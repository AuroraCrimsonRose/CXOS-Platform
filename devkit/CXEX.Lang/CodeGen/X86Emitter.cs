using System.Collections.Generic;
using System.Text;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Sema;

namespace CXEX.Lang.CodeGen;

/// <summary>
/// X core v0.1 backend: lowers the typed AST to x86-32 GAS (AT&T) assembly text,
/// which the build assembles+links (i686-elf) to an ELF, then ElfParser/CXEXWriter
/// package into a .xuex/.xsex/.xoex. Straightforward stack-machine codegen, cdecl ABI:
/// args pushed right-to-left, result in eax, callee saves ebp. The __syscall
/// intrinsic lowers to the CXK ABI register layout + int $0x80.
/// </summary>
public sealed class X86Emitter
{
    private readonly SemaContext _ctx;
    private readonly IReadOnlyDictionary<LetStmt, TypeRef> _localTypes;
    private readonly DiagnosticBag _diag;
    private readonly StringBuilder _text = new();
    private readonly StringBuilder _data = new();
    private readonly StringBuilder _bss  = new();
    private int _label;

    // current function frame: name -> (ebp offset, type)
    private Dictionary<string, (int off, TypeRef ty)> _frame = new();

    /* Wide values live at an address, so a wide expression needs somewhere to
       put its result. A stack-machine emitter nests expressions to a bounded
       depth, so a small pool of 16-byte slots in the frame is enough: entering
       a wide operation takes the next slot, leaving it gives the slot back.
       Exceeding the pool is a diagnostic, never silent reuse. */
    private const int WideTempSlots = 8;
    private const int WideTempSize  = 16;
    private int _wideTempBase = 0;    // frame offset of slot 0
    private int _wideDepth    = 0;    // slots currently in use

    public X86Emitter(SemaContext ctx, IReadOnlyDictionary<LetStmt, TypeRef> localTypes, DiagnosticBag diag)
    { _ctx = ctx; _localTypes = localTypes; _diag = diag; }

    private string NL() => $".L{_label++}";

    // string literal pool: identical strings share one label; bytes go to .data,
    // NUL-terminated, so a string is usable both as a counted buffer and a C-style
    // NUL-terminated string (which is what SYS_CONSOLE_WRITE with len=0 expects).
    private readonly Dictionary<string, string> _strings = new();
    private string InternString(string value)
    {
        if (_strings.TryGetValue(value, out var lbl)) return lbl;
        lbl = $".Lstr{_strings.Count}";
        _strings[value] = lbl;
        _data.AppendLine($"{lbl}:");
        _data.AppendLine($"    .byte {BytesOf(value)}");
        return lbl;
    }

    // emit the string as a comma-separated byte list + trailing 0. ASCII/UTF-8.
    private static string BytesOf(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s);
        var sb = new StringBuilder();
        foreach (var b in bytes) { sb.Append(b); sb.Append(", "); }
        sb.Append('0');   // NUL terminator
        return sb.ToString();
    }
    private void T(string s) => _text.AppendLine("    " + s);
    private void Lbl(string l) => _text.AppendLine(l + ":");

    public string Emit(CompilationUnit unit)
    {
        _text.AppendLine(".text");
        _text.AppendLine(".globl _start");
        _data.AppendLine(".data");
        _bss.AppendLine(".bss");
        foreach (var d in unit.Decls)
        {
            if (d is FnDecl f && f.Body != null) EmitFn(f);
            else if (d is GlobalDecl g) EmitGlobal(g);
        }
        return _text + "\n" + _data + "\n" + _bss;
    }

    // ---- type sizes / struct layout (v0.1: every field 4-aligned, matches ABI structs) ----
    /* ---- sized memory access ----
       Emitting a 32-bit access for every load/store corrupts u8/u16 data:
       reading buf[i] from a [N]u8 pulled 4 bytes, writing it clobbered 3
       neighbours. These use the operand's real width. */
    /* Expands aliases first, like SizeOf and StructOf: without it `type fx = i32;`
       reads as unsigned, so division emits div instead of idiv, >> emits shr
       instead of sar, and a 1-byte load zero-extends where it should sign-extend. */
    private bool IsSigned(TypeRef t0) =>
        _ctx.Expand(t0) is PrimType p && PrimWidth.IsSigned(p.Kind);

    /* A value too wide for one register. These follow the same path structs
       and arrays already take - the expression yields an ADDRESS in eax rather
       than the value - which is why the emitter needs no second value model
       and why a wider type later is a table row rather than a rewrite. */
    private bool IsWide(TypeRef t0) =>
        _ctx.Expand(t0) is PrimType p && PrimWidth.IsWide(p.Kind);

    private void LoadFrom(TypeRef t)
    {
        /* A wide value is not loaded - eax already holds its address and that
           IS the value's representation, exactly as it is for a struct. */
        if (IsWide(t)) return;
        switch (SizeOf(t))
        {
            case 1: T(IsSigned(t) ? "movsbl (%eax), %eax" : "movzbl (%eax), %eax"); break;
            case 2: T(IsSigned(t) ? "movswl (%eax), %eax" : "movzwl (%eax), %eax"); break;
            default: T("mov (%eax), %eax"); break;
        }
    }

    /* Copy `bytes` from the address in esi to the address in edi, a word at a
       time. Used for assigning a wide value or a struct - both are "the thing
       at this address", and a 4-byte store would have copied a quarter of a
       u128 and the first field of a struct. */
    private void CopyBytes(int bytes)
    {
        for (int o = 0; o < bytes; o += 4)
        {
            T($"mov {o}(%esi), %eax");
            T($"mov %eax, {o}(%edi)");
        }
    }

    /* esi, edi and ebx are callee-saved in cdecl. The rest of this emitter
       never touches them, so it complies by accident; the wide-value code
       needs three pointers at once and has to comply on purpose. */
    private void SaveIdx() { T("push %esi"); T("push %edi"); }
    private void RestoreIdx() { T("pop %edi"); T("pop %esi"); }

    private void StoreTo(TypeRef t)
    {
        switch (SizeOf(t))
        {
            case 1: T("mov %cl, (%eax)"); break;
            case 2: T("mov %cx, (%eax)"); break;
            default: T("mov %ecx, (%eax)"); break;
        }
    }

    private TypeRef TypeOf(Expr e) =>
        _ctx.Types.TryGetValue(e, out var t) ? t : new PrimType(PrimKind.I32);

    private int SizeOf(TypeRef t0) => _ctx.Expand(t0) switch
    {
        PrimType p => PrimWidth.Bytes(p.Kind),
        PointerType => 4,
        FuncType => 4,                    // a function pointer is an address
        ArrayType a => SizeOf(a.Element) * a.Length,   /* NOT Align4: [N]u8 = N contiguous bytes */
        NamedType n when _ctx.Structs.TryGetValue(n.Name, out var s) => StructSize(s),
        _ => 4
    };
    private static int Align4(int n) => (n + 3) & ~3;
    private int StructSize(StructDecl s) { int o = 0; foreach (var f in s.Fields) o += Align4(SizeOf(f.Type)); return o; }
    private int FieldOffset(StructDecl s, string field)
    { int o = 0; foreach (var f in s.Fields) { if (f.Name == field) return o; o += Align4(SizeOf(f.Type)); } return 0; }
    private StructDecl? StructOf(TypeRef t0) { var t = _ctx.Expand(t0); return t is NamedType n && _ctx.Structs.TryGetValue(n.Name, out var s) ? s : null; }

    // ---- functions ----
    private void EmitFn(FnDecl f)
    {
        _frame = new();
        _curFn = f.Name;
        // params: [ebp+8], [ebp+12], ... (cdecl, all 4-byte slots in v0.1)
        int poff = 8;
        foreach (var p in f.Params) { _frame[p.Name] = (poff, p.Type); poff += 4; }
        // locals: assign descending offsets; size from sema
        int locals = 0;
        foreach (var l in CollectLocals(f.Body!))
        {
            var ty = _localTypes.TryGetValue(l, out var t) ? t : new PrimType(PrimKind.I32);
            locals += Align4(SizeOf(ty));
            _frame[l.Name] = (-locals, ty);
        }

        /* Reserve the wide-temp pool only for functions that actually use a
           wide type - every other frame stays exactly the size it was. */
        _wideDepth = 0;
        _wideTempBase = 0;
        if (UsesWide(f))
        {
            locals += WideTempSlots * WideTempSize;
            _wideTempBase = -locals;
        }

        Lbl(f.Name);
        T("push %ebp");
        T("mov %esp, %ebp");
        if (locals > 0) T($"sub ${Align4(locals)}, %esp");
        EmitBlock(f.Body!);
        Lbl(f.Name + "$ret");
        T("mov %ebp, %esp");
        T("pop %ebp");
        T("ret");
    }

    private IEnumerable<LetStmt> CollectLocals(Block b)
    {
        foreach (var s in b.Stmts)
            foreach (var l in LocalsIn(s)) yield return l;
    }
    private IEnumerable<LetStmt> LocalsIn(Stmt s)
    {
        switch (s)
        {
            case LetStmt l: yield return l; break;
            case Block b: foreach (var x in CollectLocals(b)) yield return x; break;
            case IfStmt i:
                foreach (var x in CollectLocals(i.Then)) yield return x;
                if (i.Else != null) foreach (var x in CollectLocals(i.Else)) yield return x; break;
            case WhileStmt w: foreach (var x in CollectLocals(w.Body)) yield return x; break;
        }
    }

    /* A global with no initializer, or one that folds to zero, goes to .bss.
     *
     * Every global used to be emitted as explicit .long words in .data, so a
     * zero-filled array was carried byte for byte: the linker put it in the
     * file, the CXEX image carried it, and app_image.h embedded it in the
     * executive. A 32KB buffer cost 32KB in every one of those places to say
     * nothing at all. The CXEX loader already hands out zeroed pages
     * (cxex_load.c: "page arrives zeroed (BSS-ready)"), so .bss costs nothing
     * on disk and arrives zeroed anyway - which is what the language promises
     * an uninitialized global.
     *
     * Only genuinely non-zero initializers still take space in .data.
     */
    private void EmitGlobal(GlobalDecl g)
    {
        int size = Align4(SizeOf(g.Type));
        ulong init = 0;
        if (g.Init != null) new ConstFold(_ctx, _diag).TryEval(g.Init, out init);

        if (init == 0)
        {
            _bss.AppendLine("    .align 4");
            _bss.AppendLine($"{g.Name}:");
            _bss.AppendLine($"    .zero {size}");
            return;
        }

        _data.AppendLine($"{g.Name}:");
        _data.AppendLine($"    .long {init}");
        for (int i = 4; i < size; i += 4) _data.AppendLine("    .long 0");
    }

    // ---- statements ----
    private void EmitBlock(Block b) { foreach (var s in b.Stmts) EmitStmt(s); }

    private void EmitStmt(Stmt s)
    {
        switch (s)
        {
            case Block b: EmitBlock(b); break;
            case LetStmt l:
                // no initializer -> frame slot already reserved; emit nothing
                if (l.Init != null)
                {
                    var lty = _localTypes.TryGetValue(l, out var lt0) ? lt0 : new PrimType(PrimKind.I32);
                    if (IsWide(lty)) EmitWideInit(l.Name, l.Init, lty);
                    else { EmitExpr(l.Init); StoreToVar(l.Name); }
                }
                break;
            case AssignStmt a: EmitAssign(a); break;
            case ExprStmt e: EmitExpr(e.Expr); break;
            case ReturnStmt r:
                if (r.Value != null)
                {
                    /* A wide value IS an address, and returning one means
                       returning the address of a temporary or a local in a
                       frame that is about to be torn down. The caller would
                       read whatever the next call puts there.
                       Proper support needs a caller-provided slot, the way a
                       struct return works. Until then this is a diagnostic:
                       a dangling pointer that usually happens to work is far
                       worse than a compile error. */
                    if (IsWide(TypeOf(r.Value)))
                        _diag.Error("returning a wide value is not supported yet; " +
                                    "pass a pointer to the destination instead", r.Span);
                    EmitExpr(r.Value);
                }
                T($"jmp {CurFnRet}");
                break;
            case IfStmt i:
                {
                    string els = NL(), end = NL();
                    EmitExpr(i.Cond); T("test %eax, %eax"); T($"jz {els}");
                    EmitBlock(i.Then); T($"jmp {end}");
                    Lbl(els); if (i.Else != null) EmitBlock(i.Else);
                    Lbl(end); break;
                }
            case WhileStmt w:
                {
                    string top = NL(), end = NL();
                    Lbl(top); EmitExpr(w.Cond); T("test %eax, %eax"); T($"jz {end}");
                    _loops.Push((top, end));
                    EmitBlock(w.Body);
                    _loops.Pop();
                    T($"jmp {top}"); Lbl(end); break;
                }

            case BreakStmt:
                if (_loops.Count > 0) T($"jmp {_loops.Peek().end}");
                break;

            case ContinueStmt:
                // jump to the loop top: re-tests the condition, as in C
                if (_loops.Count > 0) T($"jmp {_loops.Peek().top}");
                break;
        }
    }

    private string CurFnRet => _curFn + "$ret";
    private string _curFn = "";

    // enclosing loops: (continue target, break target). WhileStmt pushes before
    // emitting its body so break/continue inside know where to jump.
    private readonly Stack<(string top, string end)> _loops = new();

    private void StoreToVar(string name)
    {
        var (off, _) = _frame[name];
        T($"mov %eax, {off}(%ebp)");
    }

    /* Initialise a wide local by COPYING the value into its slot. Storing what
       eax holds would store the address of a temporary - which is exactly what
       the first version did, leaving `c` holding a pointer to the sum rather
       than the sum. */
    private void EmitWideInit(string name, Expr init, TypeRef wt)
    {
        int saved = _wideDepth;
        EmitWideOperand(init, wt);          // source address -> eax
        SaveIdx();
        T("mov %eax, %esi");
        T($"lea {_frame[name].off}(%ebp), %edi");
        CopyBytes(SizeOf(wt));
        RestoreIdx();
        _wideDepth = saved;
    }

    private void EmitAssign(AssignStmt a)
    {
        var tt = TypeOf(a.Target);

        /* Assigning to an existing wide variable is the same copy. */
        if (IsWide(tt))
        {
            int saved = _wideDepth;
            EmitWideOperand(a.Value, tt);   // source address -> eax
            T("push %eax");
            EmitAddr(a.Target);             // destination address -> eax
            SaveIdx();
            T("mov %eax, %edi");
            T("mov 8(%esp), %esi");
            CopyBytes(SizeOf(tt));
            RestoreIdx();
            T("add $4, %esp");
            _wideDepth = saved;
            return;
        }

        /* A wide value - or a struct - is at an address, so assigning it is a
           copy of its bytes rather than a register store. */
        if (IsWide(tt) || StructOf(tt) != null)
        {
            EmitExpr(a.Value);       // source ADDRESS -> eax
            T("push %eax");
            EmitAddr(a.Target);      // destination address -> eax
            SaveIdx();
            T("mov %eax, %edi");
            T("mov 8(%esp), %esi");  // the pushed source, now under two saves
            CopyBytes(SizeOf(tt));
            RestoreIdx();
            T("add $4, %esp");       // drop the pushed source
            return;
        }

        EmitExpr(a.Value);       // value -> eax
        T("push %eax");
        EmitAddr(a.Target);      // address -> eax
        T("pop %ecx");           // value -> ecx
        StoreTo(TypeOf(a.Target));   // sized store
    }

    // ---- expressions: result in eax ----
    private void EmitExpr(Expr e)
    {
        switch (e)
        {
            case IntLit i: T($"mov ${i.Value}, %eax"); break;
            case StrLit s: T($"mov ${InternString(s.Value)}, %eax"); break;   // address of pooled bytes
            case SizeofExpr sz: T($"mov ${SizeOf(TypeOf(sz.Operand))}, %eax"); break;   // compile-time; operand not evaluated
            case BoolLit b: T($"mov ${(b.Value ? 1 : 0)}, %eax"); break;
            case NameExpr n: EmitName(n); break;
            case BinaryExpr b: EmitBinary(b); break;
            case UnaryExpr u: EmitUnary(u); break;
            case CallExpr c: EmitCall(c); break;
            case CastExpr c: EmitCast(c); break;
            case MemberExpr or IndexExpr: EmitAddr(e); LoadFrom(TypeOf(e)); break; // load value at field/elem (sized)
            default: T("xor %eax, %eax"); break;
        }
    }

    private void EmitName(NameExpr n)
    {
        if (!_ctx.Resolved.TryGetValue(n, out var sym)) { T("xor %eax, %eax"); return; }
        if (sym.Kind == SymKind.Const && sym.Decl is ConstDecl cd && new ConstFold(_ctx, _diag).TryEval(cd.Value, out var v))
        { T($"mov ${v}, %eax"); return; }
        // a function used as a value yields its address (function pointer)
        if (sym.Kind == SymKind.Function) { T($"mov ${n.Name}, %eax"); return; }
        if (sym.Kind == SymKind.Global)
        {
            /* A wide global yields its ADDRESS, same as a struct or array -
               the value does not fit in the register, and its address is how
               the value is represented everywhere else. */
            if (sym.Decl is GlobalDecl gv && IsWide(gv.Type)) T($"mov ${n.Name}, %eax");
            else T($"mov {n.Name}, %eax");
            return;
        }
        if (_frame.TryGetValue(n.Name, out var slot))
        {
            // struct/array/wide names yield their address (decay); scalars load value
            if (_ctx.Expand(slot.ty) is NamedType or ArrayType || IsWide(slot.ty))
                T($"lea {slot.off}(%ebp), %eax");
            else T($"mov {slot.off}(%ebp), %eax");
        }
        else T("xor %eax, %eax");
    }

    // address of an lvalue -> eax
    private void EmitAddr(Expr e)
    {
        switch (e)
        {
            case NameExpr n:
                if (_frame.TryGetValue(n.Name, out var slot)) T($"lea {slot.off}(%ebp), %eax");
                else if (_ctx.Resolved.TryGetValue(n, out var sym) && sym.Kind == SymKind.Global) T($"lea {n.Name}, %eax");
                else T("xor %eax, %eax");
                break;
            case UnaryExpr { Op: UnOp.Deref } u: EmitExpr(u.Operand); break;   // *p : address is p's value
            case MemberExpr m:
                {
                    var tt = _ctx.Types.TryGetValue(m.Target, out var t) ? t : new PrimType(PrimKind.Void);
                    var sd = StructOf(tt is PointerType p ? p.Pointee : tt);
                    if (tt is PointerType) EmitExpr(m.Target);   // ps.f : base = pointer value
                    else EmitAddr(m.Target);                     // s.f  : base = address of s
                    if (sd != null) { int fo = FieldOffset(sd, m.Field); if (fo != 0) T($"add ${fo}, %eax"); }
                    break;
                }
            case IndexExpr ix:
                {
                    var tt = _ctx.Types.TryGetValue(ix.Target, out var t) ? t : new PrimType(PrimKind.Void);
                    /* one expansion for both decisions: an aliased array (`type row_t = [64]u8;`)
                       must decay to its address like a bare array, not be loaded as a pointer. */
                    var te = _ctx.Expand(tt);
                    int es = te switch { PointerType p => SizeOf(p.Pointee), ArrayType a => SizeOf(a.Element), _ => 4 };
                    if (te is ArrayType) EmitAddr(ix.Target); else EmitExpr(ix.Target);
                    T("push %eax"); EmitExpr(ix.Index);
                    if (es != 1) T($"imul ${es}, %eax"); T("pop %ecx"); T("add %ecx, %eax");
                    break;
                }
            default: T("xor %eax, %eax"); break;
        }
    }

    /* A cast between widths has to MOVE bytes, not reinterpret a register.
     *
     * Narrow -> wide widens into a temp, sign- or zero-filling the rest.
     * Wide -> narrow takes the low word. Wide -> wide of a different size
     * copies what fits.
     *
     * Before this, `x as u128` left a 32-bit VALUE in eax and everything
     * downstream treated it as an address - a cast that silently produced a
     * wild pointer. */
    private void EmitCast(CastExpr c)
    {
        var to = TypeOf(c);
        var from = TypeOf(c.Operand);

        if (IsWide(to))
        {
            if (!IsWide(from)) { EmitWideOperand(c.Operand, to); return; }
            if (SizeOf(from) >= SizeOf(to)) { EmitExpr(c.Operand); return; }  // truncate: same address
            // widening one wide type into a larger one
            int saved = _wideDepth;
            EmitExpr(c.Operand);                 // source address -> eax
            int slot = TakeWideSlot();
            if (slot == int.MinValue) { _diag.Error("wide expression nested too deeply", c.Span); return; }
            SaveIdx();
            T("mov %eax, %esi");
            T($"lea {slot}(%ebp), %edi");
            CopyBytes(SizeOf(from));
            T(IsSigned(from) ? $"mov {SizeOf(from) - 4}(%esi), %eax" : "xor %eax, %eax");
            if (IsSigned(from)) T("sar $31, %eax");
            for (int o = SizeOf(from); o < SizeOf(to); o += 4) T($"mov %eax, {o}(%edi)");
            RestoreIdx();
            T($"lea {slot}(%ebp), %eax");
            _wideDepth = saved + 1;
            return;
        }

        if (IsWide(from)) { EmitExpr(c.Operand); T("mov (%eax), %eax"); return; }  // low word
        EmitExpr(c.Operand);                      // narrow -> narrow: reinterpret
    }

    private void EmitUnary(UnaryExpr u)
    {
        var ut = TypeOf(u);
        if (IsWide(ut) && u.Op is UnOp.Neg or UnOp.BitNot) { EmitWideUnary(u, ut); return; }

        switch (u.Op)
        {
            case UnOp.Neg: EmitExpr(u.Operand); T("neg %eax"); break;
            case UnOp.Not: EmitExpr(u.Operand); T("test %eax, %eax"); T("sete %al"); T("movzbl %al, %eax"); break;
            case UnOp.BitNot: EmitExpr(u.Operand); T("not %eax"); break;
            case UnOp.Deref: EmitExpr(u.Operand); LoadFrom(TypeOf(u)); break;   // sized
            case UnOp.AddrOf: EmitAddr(u.Operand); break;
        }
    }

    /* ~x is a per-word not. -x is 0 - x, so it reuses the borrow chain rather
       than needing its own: negating word-by-word with `neg` would lose the
       borrow between words. */
    private void EmitWideUnary(UnaryExpr u, TypeRef wt)
    {
        int words = SizeOf(wt) / 4;
        int saved = _wideDepth;
        EmitWideOperand(u.Operand, wt);         // &operand -> eax
        int slot = TakeWideSlot();
        if (slot == int.MinValue) { _diag.Error("wide expression nested too deeply", u.Span); return; }

        SaveIdx();
        T("mov %eax, %esi");
        T($"lea {slot}(%ebp), %edx");

        if (u.Op == UnOp.BitNot)
        {
            for (int i = 0; i < words; i++)
            {
                int o = i * 4;
                T($"mov {o}(%esi), %eax");
                T("not %eax");
                T($"mov %eax, {o}(%edx)");
            }
        }
        else
        {
            for (int i = 0; i < words; i++)
            {
                int o = i * 4;
                /* `mov $0` and not `xor`: xor CLEARS the carry flag, which
                   would break the borrow chain between words and make every
                   word above the first wrong by one. mov leaves flags alone. */
                T("mov $0, %eax");
                if (i == 0) T($"sub {o}(%esi), %eax");
                else        T($"sbb {o}(%esi), %eax");
                T($"mov %eax, {o}(%edx)");
            }
        }

        T("mov %edx, %eax");
        RestoreIdx();
        _wideDepth = saved + 1;
    }

    /* Does this function mention a wide type anywhere - a local, a parameter,
       or the type of any expression sema resolved? Decides whether to reserve
       the temp pool, so narrow functions pay nothing. */
    private bool UsesWide(FnDecl f)
    {
        foreach (var p in f.Params) if (IsWide(p.Type)) return true;
        foreach (var l in CollectLocals(f.Body!))
            if (_localTypes.TryGetValue(l, out var t) && IsWide(t)) return true;
        foreach (var kv in _ctx.Types) if (IsWide(kv.Value)) return true;
        return false;
    }

    private static bool IsCompare(BinOp op) =>
        op is BinOp.Eq or BinOp.Ne or BinOp.Lt or BinOp.Le or BinOp.Gt or BinOp.Ge;

    private void EmitBinary(BinaryExpr b)
    {
        if (b.Op is BinOp.And or BinOp.Or) { EmitShortCircuit(b); return; }

        /* Wide operands take a different path entirely: the values are at
           addresses, not in registers. A comparison is decided by its OPERAND
           type - it yields a narrow bool - so both are checked. */
        if (IsWide(TypeOf(b)) || (IsCompare(b.Op) && IsWide(TypeOf(b.Left))))
        { EmitWideBinary(b); return; }

        EmitExpr(b.Right); T("push %eax");
        EmitExpr(b.Left); T("pop %ecx");   // left in eax, right in ecx
        switch (b.Op)
        {
            case BinOp.Add: T("add %ecx, %eax"); break;
            case BinOp.Sub: T("sub %ecx, %eax"); break;
            case BinOp.Mul: T("imul %ecx, %eax"); break;
            // signed operands use cdq+idiv; unsigned zero-extend into edx and use div.
            // Using idiv on u32 silently produces garbage above 2^31.
            case BinOp.Div:
                if (IsSigned(TypeOf(b.Left))) { T("cdq"); T("idiv %ecx"); }
                else { T("xor %edx, %edx"); T("div %ecx"); }
                break;
            case BinOp.Mod:
                if (IsSigned(TypeOf(b.Left))) { T("cdq"); T("idiv %ecx"); }
                else { T("xor %edx, %edx"); T("div %ecx"); }
                T("mov %edx, %eax");
                break;
            case BinOp.BitAnd: T("and %ecx, %eax"); break;
            case BinOp.BitOr:  T("or %ecx, %eax"); break;
            case BinOp.BitXor: T("xor %ecx, %eax"); break;
            // x86 shifts take the count in %cl; the right operand is already in %ecx.
            // >> is arithmetic (sar) for signed operands, logical (shr) for unsigned.
            case BinOp.Shl: T("shl %cl, %eax"); break;
            case BinOp.Shr: T(IsSigned(TypeOf(b.Left)) ? "sar %cl, %eax" : "shr %cl, %eax"); break;
            case BinOp.Eq: Cmp("sete"); break;
            case BinOp.Ne: Cmp("setne"); break;
            case BinOp.Lt: Cmp("setl"); break;
            case BinOp.Le: Cmp("setle"); break;
            case BinOp.Gt: Cmp("setg"); break;
            case BinOp.Ge: Cmp("setge"); break;
        }
    }
    private void Cmp(string setcc) { T("cmp %ecx, %eax"); T($"{setcc} %al"); T("movzbl %al, %eax"); }

    /* ---- wide (64/128-bit) binary operations ----
     *
     * Both operands arrive as ADDRESSES. Registers during the sequence:
     *   esi = &left   edi = &right   edx = &result   eax, ecx = scratch
     * esi and edi are callee-saved and are bracketed by SaveIdx/RestoreIdx.
     *
     * Nothing between the arithmetic instructions touches the flags - mov and
     * lea both leave them alone - so the carry chain survives the stores that
     * sit between the adds.
     */
    private void EmitWideBinary(BinaryExpr b)
    {
        var lt = TypeOf(b.Left);
        int n = SizeOf(lt);                     // 8 or 16
        int words = n / 4;
        bool signed = IsSigned(lt);

        int savedDepth = _wideDepth;

        EmitWideOperand(b.Right, lt); T("push %eax");   // &right
        EmitWideOperand(b.Left,  lt); T("pop %ecx");    // eax = &left, ecx = &right

        SaveIdx();
        T("mov %eax, %esi");
        T("mov %ecx, %edi");

        if (IsCompare(b.Op))
        { EmitWideCompare(b.Op, words, signed); RestoreIdx(); _wideDepth = savedDepth; return; }

        int slot = TakeWideSlot();
        if (slot == int.MinValue)
        {
            _diag.Error("wide expression nested too deeply; split it into steps", b.Span);
            RestoreIdx(); _wideDepth = savedDepth; T("xor %eax, %eax"); return;
        }
        T($"lea {slot}(%ebp), %edx");

        switch (b.Op)
        {
            case BinOp.Add: EmitWideAddSub("add", "adc", words); break;
            case BinOp.Sub: EmitWideAddSub("sub", "sbb", words); break;
            case BinOp.BitAnd: EmitWideBitwise("and", words); break;
            case BinOp.BitOr:  EmitWideBitwise("or",  words); break;
            case BinOp.BitXor: EmitWideBitwise("xor", words); break;
            case BinOp.Mul: EmitWideMul(words); break;
            case BinOp.Shl: EmitWideShift(b, words, left: true, signed); break;
            case BinOp.Shr: EmitWideShift(b, words, left: false, signed); break;
            default:
                /* Divide and modulo are not emitted yet. A diagnostic rather
                   than a wrong answer: silently returning the low word of a
                   quotient is exactly the bug wide types were added to
                   prevent. */
                _diag.Error($"'{b.Op}' is not implemented for {n * 8}-bit operands yet", b.Span);
                break;
        }

        T("mov %edx, %eax");                    // the result IS its address
        RestoreIdx();
        /* The result slot stays taken until the enclosing expression is done
           with it - the caller restores the depth, not this frame. */
        _wideDepth = savedDepth + 1;
    }

    /* Leave the ADDRESS of a wide value of type `wt` in eax.
     *
     * A wide source already evaluates to an address. A narrow one - most often
     * an integer literal - is widened into a temp slot first: the low word
     * from the value, the rest filled by sign-extension for a signed type and
     * zero for an unsigned one. Without this, `let a: u64 = 1;` wrote one word
     * and left the other holding whatever was on the stack. */
    private void EmitWideOperand(Expr src, TypeRef wt)
    {
        if (IsWide(TypeOf(src))) { EmitExpr(src); return; }

        int n = SizeOf(wt);
        int slot = TakeWideSlot();
        if (slot == int.MinValue) { T("xor %eax, %eax"); return; }

        if (src is IntLit il)
        {
            for (int o = 0; o < n; o += 4)
            {
                uint w = o < 8 ? (uint)((il.Value >> (o * 8)) & 0xFFFFFFFFul) : 0u;
                T($"movl ${w}, {slot + o}(%ebp)");
            }
        }
        else
        {
            EmitExpr(src);                       // narrow value -> eax
            T($"mov %eax, {slot}(%ebp)");
            if (IsSigned(wt)) T("sar $31, %eax");   // replicate the sign bit
            else              T("xor %eax, %eax");
            for (int o = 4; o < n; o += 4) T($"mov %eax, {slot + o}(%ebp)");
        }
        T($"lea {slot}(%ebp), %eax");
    }

    /* Slots are taken in a stack discipline: the caller saves _wideDepth,
       takes what it needs, and restores. Freeing per-operand instead would
       hand the same slot to both sides of a binary operation and the second
       would overwrite the first. */
    private int TakeWideSlot()
    {
        if (_wideDepth >= WideTempSlots) return int.MinValue;
        return _wideTempBase + _wideDepth++ * WideTempSize;
    }

    private void EmitWideAddSub(string first, string rest, int words)
    {
        for (int i = 0; i < words; i++)
        {
            int o = i * 4;
            T($"mov {o}(%esi), %eax");
            T($"{(i == 0 ? first : rest)} {o}(%edi), %eax");
            T($"mov %eax, {o}(%edx)");
        }
    }

    /* Schoolbook multiply, truncated to the operand width - what `imul` does
     * for 32 bits, one limb at a time.
     *
     * For each pair of limbs whose product lands inside the result, `mul`
     * gives a 64-bit product in edx:eax which is added in at position i+j and
     * carried upward. Pairs already past the top are skipped rather than
     * computed and thrown away.
     *
     * The destination pointer moves to ebx because `mul` clobbers edx. ebx is
     * callee-saved, so it is pushed and popped around the sequence.
     */
    private void EmitWideMul(int words)
    {
        T("push %ebx");
        T("mov %edx, %ebx");                       // ebx = &dst, edx now free

        for (int i = 0; i < words; i++) T($"movl $0, {i * 4}(%ebx)");

        for (int i = 0; i < words; i++)
        {
            for (int j = 0; i + j < words; j++)
            {
                T($"mov {i * 4}(%esi), %eax");
                T($"mull {j * 4}(%edi)");          // edx:eax = lhs[i] * rhs[j]
                T($"add %eax, {(i + j) * 4}(%ebx)");
                for (int k = i + j + 1; k < words; k++)
                    T(k == i + j + 1 ? $"adc %edx, {k * 4}(%ebx)"
                                     : $"adcl $0, {k * 4}(%ebx)");
            }
        }

        T("mov %ebx, %edx");                       // hand the pointer back
        T("pop %ebx");
    }

    /* Shift by a constant. shld/shrd do the across-limb part in one
     * instruction each: shld pulls in the top bits of the neighbour below,
     * shrd the bottom bits of the neighbour above.
     *
     * A VARIABLE shift is deliberately not emitted. It needs a loop, and a
     * loop whose trip count depends on the operand is a timing signal - the
     * same reason the comparison here would be unsafe on secret data. Anything
     * that wants one can shift in a loop it wrote itself and can see.
     */
    private void EmitWideShift(BinaryExpr b, int words, bool left, bool signed)
    {
        if (!new ConstFold(_ctx, _diag).TryEval(b.Right, out var amt))
        {
            _diag.Error("a wide shift needs a constant amount", b.Span);
            return;
        }

        int bits = (int)amt;
        int total = words * 32;
        if (bits < 0 || bits >= total)
        {
            _diag.Error($"shift of {bits} is outside 0..{total - 1} for this width", b.Span);
            return;
        }

        int wordShift = bits / 32, bitShift = bits % 32;

        if (left)
        {
            for (int k = words - 1; k >= 0; k--)
            {
                int src = k - wordShift;
                if (src < 0) { T($"movl $0, {k * 4}(%edx)"); continue; }
                T($"mov {src * 4}(%esi), %eax");
                if (bitShift != 0)
                {
                    if (src - 1 >= 0) { T($"mov {(src - 1) * 4}(%esi), %ecx"); T($"shld ${bitShift}, %ecx, %eax"); }
                    else T($"shl ${bitShift}, %eax");
                }
                T($"mov %eax, {k * 4}(%edx)");
            }
        }
        else
        {
            for (int k = 0; k < words; k++)
            {
                int src = k + wordShift;
                if (src >= words)
                {
                    /* Past the top: zero for a logical shift, the sign bit
                       repeated for an arithmetic one. */
                    if (signed) { T($"mov {(words - 1) * 4}(%esi), %eax"); T("sar $31, %eax"); T($"mov %eax, {k * 4}(%edx)"); }
                    else T($"movl $0, {k * 4}(%edx)");
                    continue;
                }
                T($"mov {src * 4}(%esi), %eax");
                if (bitShift != 0)
                {
                    if (src + 1 < words) { T($"mov {(src + 1) * 4}(%esi), %ecx"); T($"shrd ${bitShift}, %ecx, %eax"); }
                    else T(signed ? $"sar ${bitShift}, %eax" : $"shr ${bitShift}, %eax");
                }
                T($"mov %eax, {k * 4}(%edx)");
            }
        }
    }

    private void EmitWideBitwise(string op, int words)
    {
        for (int i = 0; i < words; i++)
        {
            int o = i * 4;
            T($"mov {o}(%esi), %eax");
            T($"{op} {o}(%edi), %eax");
            T($"mov %eax, {o}(%edx)");
        }
    }

    /* Equality is a XOR of every word OR'd together - one pass, no branches.
     * Ordering walks from the most significant word down and stops at the
     * first difference.
     *
     * A signed comparison biases only the TOP word by flipping its sign bit,
     * which turns the whole thing into an unsigned comparison: the lower words
     * were always unsigned, and the top word's order under that flip is the
     * signed order. That avoids a separate signed path per width. */
    private void EmitWideCompare(BinOp op, int words, bool signed)
    {
        if (op is BinOp.Eq or BinOp.Ne)
        {
            T("xor %ecx, %ecx");
            for (int i = 0; i < words; i++)
            {
                int o = i * 4;
                T($"mov {o}(%esi), %eax");
                T($"xor {o}(%edi), %eax");
                T("or %eax, %ecx");
            }
            T("test %ecx, %ecx");
            T(op == BinOp.Eq ? "sete %al" : "setne %al");
            T("movzbl %al, %eax");
            return;
        }

        string decide = NL();
        int top = (words - 1) * 4;

        if (signed)
        {
            T($"mov {top}(%esi), %eax");
            T("xor $0x80000000, %eax");
            T($"mov {top}(%edi), %ecx");
            T("xor $0x80000000, %ecx");
            T("cmp %ecx, %eax");
        }
        else
        {
            T($"mov {top}(%esi), %eax");
            T($"cmp {top}(%edi), %eax");
        }

        for (int i = words - 2; i >= 0; i--)
        {
            T($"jne {decide}");
            int o = i * 4;
            T($"mov {o}(%esi), %eax");
            T($"cmp {o}(%edi), %eax");
        }

        Lbl(decide);
        /* Always the unsigned condition codes: the bias above already folded
           signedness into the top word. */
        T(op switch
        {
            BinOp.Lt => "setb %al",
            BinOp.Le => "setbe %al",
            BinOp.Gt => "seta %al",
            _        => "setae %al",
        });
        T("movzbl %al, %eax");
    }

    private void EmitShortCircuit(BinaryExpr b)
    {
        string end = NL();
        EmitExpr(b.Left);
        if (b.Op == BinOp.And) { T("test %eax, %eax"); T($"jz {end}"); }
        else { T("test %eax, %eax"); T($"jnz {end}"); }
        EmitExpr(b.Right);
        Lbl(end);
        T("test %eax, %eax"); T("setne %al"); T("movzbl %al, %eax");
    }

    private void EmitCall(CallExpr c)
    {
        if (c.Callee is NameExpr { Name: "__syscall" }) { EmitSyscall(c); return; }
        // cdecl: push args right-to-left
        for (int i = c.Args.Count - 1; i >= 0; i--) { EmitExpr(c.Args[i]); T("push %eax"); }
        /* Only a name that resolves to a FUNCTION is a direct call. A local or global
           holding a function pointer is also a NameExpr, and emitting `call <name>`
           for it calls a symbol that does not exist - it has to go through the value. */
        if (c.Callee is NameExpr nm
            && _ctx.Resolved.TryGetValue(nm, out var csym)
            && csym.Kind == SymKind.Function) T($"call {nm.Name}");
        else { EmitExpr(c.Callee); T("call *%eax"); }
        if (c.Args.Count > 0) T($"add ${c.Args.Count * 4}, %esp");
    }

    // __syscall(n, a1..a5) -> eax=n, ebx,ecx,edx,esi,edi = a1..a5 ; int $0x80
    private void EmitSyscall(CallExpr c)
    {
        // evaluate all args, push, then pop into the right registers
        foreach (var a in c.Args) { EmitExpr(a); T("push %eax"); }   // pushed n,a1,..  (n deepest)
        string[] regs = { "%eax", "%ebx", "%ecx", "%edx", "%esi", "%edi" };
        // top of stack is the last arg; pop in reverse so eax=n ends last
        for (int i = c.Args.Count - 1; i >= 0; i--) T($"pop {regs[i]}");
        // zero any unspecified arg registers
        for (int i = c.Args.Count; i < 6; i++) T($"xor {regs[i]}, {regs[i]}");
        T("int $0x80");   // result already in eax
    }
}