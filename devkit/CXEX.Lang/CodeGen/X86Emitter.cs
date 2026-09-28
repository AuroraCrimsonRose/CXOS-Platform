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
        _ctx.Expand(t0) is PrimType p && (p.Kind == PrimKind.I8 || p.Kind == PrimKind.I16 || p.Kind == PrimKind.I32);

    private void LoadFrom(TypeRef t)
    {
        switch (SizeOf(t))
        {
            case 1: T(IsSigned(t) ? "movsbl (%eax), %eax" : "movzbl (%eax), %eax"); break;
            case 2: T(IsSigned(t) ? "movswl (%eax), %eax" : "movzwl (%eax), %eax"); break;
            default: T("mov (%eax), %eax"); break;
        }
    }

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
        PrimType p => p.Kind switch
        {
            PrimKind.I8 or PrimKind.U8 or PrimKind.Bool => 1,
            PrimKind.I16 or PrimKind.U16 => 2,
            _ => 4
        },
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
                if (l.Init != null) { EmitExpr(l.Init); StoreToVar(l.Name); }
                break;
            case AssignStmt a: EmitAssign(a); break;
            case ExprStmt e: EmitExpr(e.Expr); break;
            case ReturnStmt r:
                if (r.Value != null) EmitExpr(r.Value);
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

    private void EmitAssign(AssignStmt a)
    {
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
            case CastExpr c: EmitExpr(c.Operand); break;          // v0.1: reinterpret, no convert
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
        if (sym.Kind == SymKind.Global) { T($"mov {n.Name}, %eax"); return; }
        if (_frame.TryGetValue(n.Name, out var slot))
        {
            // struct/array names yield their address (decay); scalars load value
            if (_ctx.Expand(slot.ty) is NamedType or ArrayType) T($"lea {slot.off}(%ebp), %eax");
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

    private void EmitUnary(UnaryExpr u)
    {
        switch (u.Op)
        {
            case UnOp.Neg: EmitExpr(u.Operand); T("neg %eax"); break;
            case UnOp.Not: EmitExpr(u.Operand); T("test %eax, %eax"); T("sete %al"); T("movzbl %al, %eax"); break;
            case UnOp.BitNot: EmitExpr(u.Operand); T("not %eax"); break;
            case UnOp.Deref: EmitExpr(u.Operand); LoadFrom(TypeOf(u)); break;   // sized
            case UnOp.AddrOf: EmitAddr(u.Operand); break;
        }
    }

    private void EmitBinary(BinaryExpr b)
    {
        if (b.Op is BinOp.And or BinOp.Or) { EmitShortCircuit(b); return; }
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