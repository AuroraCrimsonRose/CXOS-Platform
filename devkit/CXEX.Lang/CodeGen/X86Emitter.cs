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

    /* ---- values that do not fit in eax, passed and returned BY VALUE ----
     * The convention is the i386 System V one - what GCC does for C - so X
     * stays callable from C and C from X:
     *   - an argument is COPIED onto the stack, its size rounded up to 4;
     *   - a 64-bit result comes back in edx:eax;
     *   - a 128-bit or struct result is written through a hidden pointer the
     *     caller pushes as the FIRST argument; the callee returns that pointer
     *     in eax and pops it itself (`ret $4`).
     * Before this, a wide or struct argument pushed its ADDRESS and the callee
     * read the address's bytes as the value, and a wide or struct result
     * returned the address of a local in a frame already gone. Both compiled
     * cleanly and gave wrong answers.
     *
     * Each call site whose result is one of these gets its own slot in the
     * caller's frame to receive it - sized for that type, so a struct of any
     * size fits - allocated when the frame is laid out. */
    private Dictionary<CallExpr, int> _callSlots = new();
    private TypeRef _curReturn = new PrimType(PrimKind.Void);
    private bool _curSret;

    private bool IsAggregate(TypeRef t) => IsWide(t) || StructOf(t) != null;
    private bool ReturnsInRegs64(TypeRef t) => IsWide(t) && SizeOf(t) == 8;
    private bool ReturnsViaPointer(TypeRef t) => StructOf(t) != null || (IsWide(t) && SizeOf(t) > 8);

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

    /* Emit a linkable object rather than a program: every function with a body
       and every global is exported, and there is no entry point. This is how
       X code is linked into something that is not an X program - the kernel
       calls std/xdata.xfxn this way. The calling convention is already cdecl
       (arguments pushed right to left, caller pops, result in eax, ebx/esi/edi
       preserved), so C can call it with an ordinary prototype. */
    public bool Library { get; init; }

    public string Emit(CompilationUnit unit)
    {
        _text.AppendLine(".text");
        if (!Library) _text.AppendLine(".globl _start");
        _data.AppendLine(".data");
        _bss.AppendLine(".bss");
        foreach (var d in unit.Decls)
        {
            if (d is FnDecl f && f.Body != null)
            {
                if (Library) _text.AppendLine($".globl {f.Name}");
                EmitFn(f);
            }
            else if (d is GlobalDecl g)
            {
                if (Library) _text.AppendLine($".globl {g.Name}");
                EmitGlobal(g);
            }
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
        _curReturn = f.Return;
        _curSret = ReturnsViaPointer(f.Return);
        /* params: [ebp+8] upward, cdecl. A wide or struct parameter occupies
           its whole size, copied there by the caller; a hidden result pointer,
           when there is one, comes first. */
        int poff = _curSret ? 12 : 8;
        foreach (var p in f.Params)
        {
            _frame[p.Name] = (poff, p.Type);
            poff += IsAggregate(p.Type) ? Align4(SizeOf(p.Type)) : 4;
        }
        // locals: assign descending offsets; size from sema
        int locals = 0;
        foreach (var l in CollectLocals(f.Body!))
        {
            var ty = _localTypes.TryGetValue(l, out var t) ? t : new PrimType(PrimKind.I32);
            locals += Align4(SizeOf(ty));
            _frame[l.Name] = (-locals, ty);
        }

        _callSlots = new();
        foreach (var c in CallsIn(f.Body!))
        {
            var rt = TypeOf(c);
            if (!IsAggregate(rt) || _callSlots.ContainsKey(c)) continue;
            locals += Align4(SizeOf(rt));
            _callSlots[c] = -locals;
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

        var section = SectionOf(f);
        if (section != null) _text.AppendLine($".section {section},\"ax\",@progbits");   // allocated + executable
        Lbl(f.Name);
        T("push %ebp");
        T("mov %esp, %ebp");
        if (locals > 0) T($"sub ${Align4(locals)}, %esp");
        EmitBlock(f.Body!);
        Lbl(f.Name + "$ret");
        T("mov %ebp, %esp");
        T("pop %ebp");
        T(_curSret ? "ret $4" : "ret");   /* the callee pops the hidden pointer - i386 SysV */
        if (section != null) _text.AppendLine(".text");   // the next function goes back where it belongs
    }

    /* The section a declaration asked for with @section, or null for the
       default. The type checker has already validated it, so this only reads. */
    private static string? SectionOf(Decl d)
    {
        foreach (var a in d.Attrs)
            if (a.Name == "section" && a.Args.Count == 1 && a.Args[0].Value is StrLit s) return s.Value;
        return null;
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
            // a `let` inside a deferred block still needs a frame slot, and the
            // frame is sized before any statement is emitted
            case DeferStmt d: foreach (var x in LocalsIn(d.Body)) yield return x; break;
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
        UInt128 init = UInt128.Zero;
        if (g.Init != null) new ConstFold(_ctx, _diag).TryEval(g.Init, out init);

        /* A global in a named section is emitted there in full, even when it
           is zero. Sending it to .bss would put it somewhere other than where
           it was asked to be - and a section read as a table must contain
           every entry, zero-valued ones included. */
        var section = SectionOf(g);
        if (section != null)
        {
            _data.AppendLine($".section {section},\"aw\",@progbits");   // allocated + writable
            _data.AppendLine("    .align 4");
            _data.AppendLine($"{g.Name}:");
            for (int i = 0; i < size; i += 4)
                _data.AppendLine($"    .long {(i < 16 ? (uint)(init >> (i * 8)) : 0u)}");
            _data.AppendLine(".data");
            return;
        }

        if (init == UInt128.Zero)
        {
            _bss.AppendLine("    .align 4");
            _bss.AppendLine($"{g.Name}:");
            _bss.AppendLine($"    .zero {size}");
            return;
        }

        /* Every word of the value, not just the low one.
         *
         * This used to emit `.long {init}` and then zeros, which was fine while
         * an initializer could not exceed 32 bits and quietly wrong the moment
         * one could: `global x: u64 = 18446744073709551615;` handed the
         * assembler a 64-bit number in a .long directive. The words come out
         * low-first because the target is little-endian, and a value narrower
         * than the global is zero-extended by the shift falling off the end. */
        _data.AppendLine("    .align 4");
        _data.AppendLine($"{g.Name}:");
        for (int i = 0; i < size; i += 4)
        {
            uint w = i < 16 ? (uint)(init >> (i * 8)) : 0u;
            _data.AppendLine($"    .long {w}");
        }
    }

    // ---- statements ----
    /* ---- defer ----
     *
     * One list of deferred statements per open block, innermost last. A
     * `defer` emits nothing where it is written; it appends its body to the
     * current block's list. Every route out of a block then emits the lists it
     * is leaving, innermost block first and, within a block, last-deferred
     * first - so cleanups run in the reverse order of the acquisitions they
     * undo, which is the whole reason to write them next to each other.
     *
     * The routes: falling off the end of a block (EmitBlock), `return` (all
     * lists, out to the function), and `break` / `continue` (only the lists
     * opened inside the loop - the loop's own enclosing blocks are not being
     * left). Each route emits its own copy of the cleanup code at the point
     * of exit. That duplicates code rather than sharing one cleanup path, and
     * is chosen deliberately: it needs no runtime state, no hidden variable
     * recording which exit was taken, and it is the shape a reader of the
     * assembly can follow.
     *
     * Only defers ALREADY REACHED run. One written after a `return` in the
     * same block has not been registered when the return is emitted, so it
     * does not run - which is correct, since whatever it would have cleaned
     * up was never acquired. */
    private readonly List<List<Stmt>> _defers = new();

    private void EmitDefers(int downTo)
    {
        for (int i = _defers.Count - 1; i >= downTo; i--)
        {
            var pending = _defers[i].ToArray();   // a copy: the bodies open blocks of their own
            for (int j = pending.Length - 1; j >= 0; j--) EmitStmt(pending[j]);
        }
    }

    private bool AnyDefers() => _defers.Exists(l => l.Count > 0);

    private void EmitBlock(Block b)
    {
        _defers.Add(new List<Stmt>());
        foreach (var s in b.Stmts) EmitStmt(s);
        EmitDefers(_defers.Count - 1);            // the fall-through exit
        _defers.RemoveAt(_defers.Count - 1);
    }

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
                    if (IsAggregate(lty)) EmitWideInit(l.Name, l.Init, lty);
                    else { EmitExpr(l.Init); StoreToVar(l.Name); }
                }
                break;
            case AssignStmt a: EmitAssign(a); break;
            case ExprStmt e: EmitExpr(e.Expr); break;
            case ReturnStmt r:
                /* A wide or struct value is at an address, and that address is
                   usually in this frame - so it is never what gets returned.
                   The value is moved out NOW, before the defers run (they
                   might change it, and `return x` returns x as it was) and
                   before the frame goes: into the caller's slot through the
                   hidden pointer, or into edx:eax for 64 bits. */
                if (r.Value != null && ReturnsViaPointer(_curReturn))
                {
                    int saved = _wideDepth;
                    if (IsWide(_curReturn)) EmitWideOperand(r.Value, _curReturn); else EmitExpr(r.Value);
                    SaveIdx();
                    T("mov %eax, %esi");
                    T("mov 8(%ebp), %edi");          // the hidden pointer
                    CopyBytes(Align4(SizeOf(_curReturn)));
                    RestoreIdx();
                    _wideDepth = saved;
                    if (AnyDefers()) EmitDefers(0);
                    T("mov 8(%ebp), %eax");          // returned, as C expects
                    T($"jmp {CurFnRet}");
                    break;
                }
                if (r.Value != null && ReturnsInRegs64(_curReturn))
                {
                    int saved = _wideDepth;
                    EmitWideOperand(r.Value, _curReturn);
                    T("pushl 4(%eax)");
                    T("pushl (%eax)");
                    _wideDepth = saved;
                    if (AnyDefers()) EmitDefers(0);
                    T("pop %eax");
                    T("pop %edx");
                    T($"jmp {CurFnRet}");
                    break;
                }
                if (r.Value != null) EmitExpr(r.Value);
                /* The value is computed BEFORE the defers run - `return x`
                   returns x as it was at the return, even if a defer then
                   changes x - and eax is saved around them because running
                   any code at all will clobber it. */
                if (AnyDefers())
                {
                    T("push %eax");
                    EmitDefers(0);
                    T("pop %eax");
                }
                T($"jmp {CurFnRet}");
                break;
            case DeferStmt d:
                if (_defers.Count > 0) _defers[^1].Add(d.Body);
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
                    _loops.Push((top, end, _defers.Count));
                    EmitBlock(w.Body);
                    _loops.Pop();
                    T($"jmp {top}"); Lbl(end); break;
                }

            case BreakStmt:
                if (_loops.Count > 0) { EmitDefers(_loops.Peek().depth); T($"jmp {_loops.Peek().end}"); }
                break;

            case ContinueStmt:
                // jump to the loop top: re-tests the condition, as in C. The
                // body's defers run first - each iteration is a fresh entry
                // into the body block, so it must be left properly each time.
                if (_loops.Count > 0) { EmitDefers(_loops.Peek().depth); T($"jmp {_loops.Peek().top}"); }
                break;
        }
    }

    private string CurFnRet => _curFn + "$ret";
    private string _curFn = "";

    // enclosing loops: (continue target, break target). WhileStmt pushes before
    // emitting its body so break/continue inside know where to jump.
    // `depth` is how many defer lists were open when the loop began: a
    // break or continue leaves every block opened since, and no others.
    private readonly Stack<(string top, string end, int depth)> _loops = new();

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
        /* A struct is at an address too, and was stored as one: `let q: pt =
           make();` put the ADDRESS of the result in q's first word. Same copy. */
        if (IsWide(wt)) EmitWideOperand(init, wt); else EmitExpr(init);   // source address -> eax
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
            /* A literal too wide for a register cannot be carried in eax, so
               it is materialised into a slot and its ADDRESS returned - the
               same representation every other wide value uses. Reaching the
               truncating line below with a wide literal would put a value
               where the caller expects an address, which is a segfault rather
               than a wrong number.

               Anything that fits is moved as an immediate, and the cast to
               uint is what keeps the assembler from being handed a number it
               cannot encode. */
            case IntLit i when IsWide(TypeOf(i)): EmitWideOperand(i, TypeOf(i)); break;
            case IntLit i: T($"mov ${(uint)i.Value}, %eax"); break;
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
        { T($"mov ${(uint)v}, %eax"); return; }   // narrow path; see IntLit above
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
        if (IsWide(TypeOf(b)) ||
            (IsCompare(b.Op) && (IsWide(TypeOf(b.Left)) || IsWide(TypeOf(b.Right)))))
        { EmitWideBinary(b); return; }

        EmitExpr(b.Right);
        /* A shift is the one narrow operation whose right operand may be wide
           (its result takes the LEFT width). A wide value is its address, so
           without this the address's low bits became the count. */
        if (b.Op is BinOp.Shl or BinOp.Shr && IsWide(TypeOf(b.Right))) T("mov (%eax), %eax");
        T("push %eax");
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
            /* Ordering needs to know the signedness, and it used to assume
               signed for everything: setl/setg on a u32 treats every value
               from 2^31 up as negative, so `x > 429496729` was FALSE for
               x = 3000000000. Division and >> above already took the operand
               type into account; comparison was the one left behind.

               The rule is C's: the comparison is unsigned if either operand is
               an unsigned 32-bit value or a pointer. A u8 or u16 widens to a
               signed int without losing any value, so it does not force it. */
            case BinOp.Lt: Cmp(UnsignedCompare(b) ? "setb"  : "setl");  break;
            case BinOp.Le: Cmp(UnsignedCompare(b) ? "setbe" : "setle"); break;
            case BinOp.Gt: Cmp(UnsignedCompare(b) ? "seta"  : "setg");  break;
            case BinOp.Ge: Cmp(UnsignedCompare(b) ? "setae" : "setge"); break;
        }
    }

    private bool UnsignedCompare(BinaryExpr b) => IsUnsignedWord(TypeOf(b.Left)) || IsUnsignedWord(TypeOf(b.Right));

    private bool IsUnsignedWord(TypeRef t0)
    {
        var t = _ctx.Expand(t0);
        if (t is PointerType or FuncType) return true;          // an address has no sign
        return t is PrimType p && !PrimWidth.IsSigned(p.Kind) && PrimWidth.Bytes(p.Kind) == 4;
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
        /* The WIDER operand sets the width, so `0 - a` widens the literal to
           match `a` rather than narrowing `a` to match the literal. A shift is
           the exception - its right operand counts places, so it does not
           widen the left. */
        var lt = TypeOf(b.Left);
        if (b.Op is not (BinOp.Shl or BinOp.Shr) && SizeOf(TypeOf(b.Right)) > SizeOf(lt))
            lt = TypeOf(b.Right);
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
            case BinOp.Div: EmitWideDivMod(words, signed, wantRemainder: false); break;
            case BinOp.Mod: EmitWideDivMod(words, signed, wantRemainder: true); break;
            default:
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
        /* A literal is materialised HERE whatever its own inferred type, and
           the check comes before the wide test on purpose. A literal too big
           for a register types wide, and sending it to EmitExpr would take the
           narrow IntLit path and hand back a VALUE in eax where the caller is
           about to dereference an ADDRESS. Widening it at `wt` is also more
           correct than widening it at its own type: the literal takes the
           width of the expression it appears in. */
        if (src is not IntLit && IsWide(TypeOf(src))) { EmitExpr(src); return; }

        int n = SizeOf(wt);
        int slot = TakeWideSlot();
        if (slot == int.MinValue) { T("xor %eax, %eax"); return; }

        if (src is IntLit il)
        {
            for (int o = 0; o < n; o += 4)
            {
                /* `o < 16`, not `o < 8`. This is where a literal stopped being
                   allowed to exceed 64 bits: the words above the eighth byte
                   were filled with zero regardless of the value, so even once
                   the lexer could read a 128-bit constant, the top half was
                   dropped on the way into the slot. */
                uint w = o < 16 ? (uint)(il.Value >> (o * 8)) : 0u;
                T($"movl ${w}, {slot + o}(%ebp)");
            }
        }
        else
        {
            EmitExpr(src);                       // narrow value -> eax
            T($"mov %eax, {slot}(%ebp)");
            /* Extended by the SOURCE's signedness, as C does and as `as`
               already did. Using the destination's made `let w: i64 = u` with
               u = 0xFFFFFFFF come out -1, and an i32 -1 widened into a u64
               came out 0x00000000FFFFFFFF. */
            if (IsSigned(TypeOf(src))) T("sar $31, %eax");   // replicate the sign bit
            else                       T("xor %eax, %eax");
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

    /* Shift by a constant amount, or by a variable one.
     *
     * A constant amount is range-checked at compile time - shifting a u64 by
     * 64 written as a literal is a mistake worth stopping. A VARIABLE amount
     * is taken modulo the width, which is what x86 already does for X's 32-bit
     * shifts: one rule for every width.
     *
     * The variable shift is a barrel shifter with no branches and no loop on
     * the amount: one stage per bit of it (6 for 64-bit, 7 for 128-bit), each
     * a constant shift by 2^k followed by a masked select that keeps or
     * discards it. The same instructions run whatever the amount is, so
     * shifting a secret by a secret does not show up in the timing - the
     * reason this used to be refused rather than written as a loop.
     */
    private void EmitWideShift(BinaryExpr b, int words, bool left, bool signed)
    {
        /* A probe, so its own diagnostics go nowhere: "not a constant" is the
           answer being asked for here, not an error. */
        if (!new ConstFold(_ctx, new DiagnosticBag()).TryEval(b.Right, out var amt))
        {
            EmitWideVarShift(b, words, left, signed);
            return;
        }
        /* Range-checked before narrowing, because the fold is 128 bits wide and
           a cast to int would wrap a huge amount into a small, plausible one. */
        if (amt > (UInt128)(words * 32))
        {
            _diag.Error($"shift of {amt} is wider than the {words * 32}-bit operand", b.Span);
            return;
        }

        int bits = (int)amt;
        int total = words * 32;
        if (bits < 0 || bits >= total)
        {
            _diag.Error($"shift of {bits} is outside 0..{total - 1} for this width", b.Span);
            return;
        }
        EmitConstShift("%esi", "%edx", words, bits, left, signed);
    }

    /* `dst` = `src` shifted by a constant `bits`. shld/shrd do the across-limb
       part in one instruction each: shld pulls in the top bits of the
       neighbour below, shrd the bottom bits of the neighbour above. eax and
       ecx are the only scratch registers, so `src` and `dst` may be any of the
       others. */
    private void EmitConstShift(string src, string dst, int words, int bits, bool left, bool signed)
    {
        int wordShift = bits / 32, bitShift = bits % 32;

        if (left)
        {
            for (int k = words - 1; k >= 0; k--)
            {
                int from = k - wordShift;
                if (from < 0) { T($"movl $0, {k * 4}({dst})"); continue; }
                T($"mov {from * 4}({src}), %eax");
                if (bitShift != 0)
                {
                    if (from - 1 >= 0) { T($"mov {(from - 1) * 4}({src}), %ecx"); T($"shld ${bitShift}, %ecx, %eax"); }
                    else T($"shl ${bitShift}, %eax");
                }
                T($"mov %eax, {k * 4}({dst})");
            }
        }
        else
        {
            for (int k = 0; k < words; k++)
            {
                int from = k + wordShift;
                if (from >= words)
                {
                    /* Past the top: zero for a logical shift, the sign bit
                       repeated for an arithmetic one. */
                    if (signed) { T($"mov {(words - 1) * 4}({src}), %eax"); T("sar $31, %eax"); T($"mov %eax, {k * 4}({dst})"); }
                    else T($"movl $0, {k * 4}({dst})");
                    continue;
                }
                T($"mov {from * 4}({src}), %eax");
                if (bitShift != 0)
                {
                    if (from + 1 < words) { T($"mov {(from + 1) * 4}({src}), %ecx"); T($"shrd ${bitShift}, %ecx, %eax"); }
                    else T(signed ? $"sar ${bitShift}, %eax" : $"shr ${bitShift}, %eax");
                }
                T($"mov %eax, {k * 4}({dst})");
            }
        }
    }

    /* On entry, as for every wide binary operation: esi = &value, edi = &amount
       (widened like any operand; only its low word matters), edx = &result.
       The result slot is worked on in place, with one more slot holding each
       stage's shifted candidate. */
    private void EmitWideVarShift(BinaryExpr b, int words, bool left, bool signed)
    {
        int tmp = TakeWideSlot();
        if (tmp == int.MinValue) { _diag.Error("wide expression nested too deeply; split it into steps", b.Span); return; }
        int width = words * 32;

        T("push %ebx");
        T("mov (%edi), %ebx");
        T($"and ${width - 1}, %ebx");              // amount mod width
        for (int i = 0; i < words; i++) { T($"mov {i * 4}(%esi), %eax"); T($"mov %eax, {i * 4}(%edx)"); }
        T("mov %edx, %esi");                       // esi = &result, shifted in place
        T($"lea {tmp}(%ebp), %edi");               // edi = &candidate

        for (int k = 0; (1 << k) < width; k++)
        {
            EmitConstShift("%esi", "%edi", words, 1 << k, left, signed);
            /* mask = all ones if bit k of the amount is set, else zero - and
               result ^= (result ^ candidate) & mask keeps one or the other
               without a branch. */
            T("mov %ebx, %edx");
            if (k > 0) T($"shr ${k}, %edx");
            T("and $1, %edx");
            T("neg %edx");
            for (int i = 0; i < words; i++)
            {
                T($"mov {i * 4}(%esi), %eax");
                T($"mov {i * 4}(%edi), %ecx");
                T("xor %eax, %ecx");
                T("and %edx, %ecx");
                T("xor %ecx, %eax");
                T($"mov %eax, {i * 4}(%esi)");
            }
        }

        T("mov %esi, %edx");                       // the caller expects &result in edx
        T("pop %ebx");
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

    /* Divide and modulo, by restoring shift-subtract long division.
     *
     * One routine produces both: a division computes the quotient and the
     * remainder together, and throwing one away would mean running the whole
     * thing twice to get `a / b` and `a % b`. `wantRemainder` only chooses
     * which of the two is copied out at the end.
     *
     * WHY THIS ALGORITHM. It is the one that is obviously correct. Knuth's
     * algorithm D is several times faster and is the right answer eventually,
     * but it is fiddly in exactly the places that produce answers that are
     * almost right - and an almost-right divide is far worse than a slow one,
     * because wide types exist here to stop silent numeric errors. The loop
     * runs once per bit: 64 or 128 iterations of a dozen instructions. That is
     * slow, and it is written down as slow rather than hidden.
     *
     * THE LOOP. Quotient and dividend share one buffer, which is the trick
     * that makes this compact: each iteration shifts the whole buffer left by
     * one, the bit falling off the top is caught by the carry flag and shifted
     * into the bottom of the remainder by the very next rcl, and the quotient
     * bit for this step is then deposited in the low bit the shift just
     * vacated. One continuous carry chain does the work of two shifts.
     *
     * Then a trial subtraction: subtract the divisor from the remainder and
     * look at the borrow. No borrow means it fitted, so the quotient bit is
     * set and the subtraction stands. A borrow means it did not, so the
     * divisor is added straight back - hence "restoring".
     *
     * SIGNS are handled outside the loop, which only ever sees magnitudes.
     * The quotient is negative when the operands disagree in sign; the
     * remainder takes the sign of the DIVIDEND, which is what truncation
     * toward zero means and what every other C-family language does, so
     * -7 % 2 is -1 and not 1.
     *
     * DIVISION BY ZERO raises #DE, by deliberately executing a 32-bit div by
     * zero. That is not a placeholder: it makes wide division behave exactly
     * as 32-bit division already does on this target, and CXK's ring-3 fault
     * handler terminates the offending process rather than the machine. A
     * software check that returned 0 instead would have invented a second,
     * quieter rule for the same mistake.
     *
     * TIMING. The trip count is fixed at the operand width, so it does not
     * leak the magnitude of either operand - but the two arms of the trial
     * subtraction are not the same length, so this is not constant time and
     * must not be used on secret values. Same caveat as the wide compare.
     *
     * Registers on entry: esi = &dividend, edi = &divisor, edx = &result.
     * edx must survive - the caller turns it into the result address - so the
     * scratch area is reached through ebx, and nothing here touches edx except
     * the divide-by-zero path, which never returns.
     */
    private void EmitWideDivMod(int words, bool signed, bool wantRemainder)
    {
        int n = words * 4;
        int bits = words * 32;

        // scratch, all reached through ebx: quotient, remainder, |divisor|,
        // and two sign flags kept out of registers because every register is
        // already spoken for.
        int quo = 0, rem = n, dvs = 2 * n, qneg = 3 * n, rneg = 3 * n + 4;
        int scratch = 3 * n + 8;

        T("push %ebx");
        T($"sub ${scratch}, %esp");
        T("mov %esp, %ebx");

        T($"movl $0, {qneg}(%ebx)");
        T($"movl $0, {rneg}(%ebx)");

        // quotient buffer starts as |dividend|, remainder starts at zero
        if (signed) EmitWideAbsTo(quo, "esi", words, qneg, alsoSet: rneg);
        else        for (int i = 0; i < words; i++)
                    { T($"mov {i * 4}(%esi), %eax"); T($"mov %eax, {quo + i * 4}(%ebx)"); }

        for (int i = 0; i < words; i++) T($"movl $0, {rem + i * 4}(%ebx)");

        // |divisor|; dividing by it flips the quotient's sign, so the flag is
        // XORed rather than assigned - it may already be set from the dividend
        if (signed) EmitWideAbsTo(dvs, "edi", words, qneg, alsoSet: -1, xorFlag: true);
        else        for (int i = 0; i < words; i++)
                    { T($"mov {i * 4}(%edi), %eax"); T($"mov %eax, {dvs + i * 4}(%ebx)"); }

        // divisor == 0 -> #DE, exactly as a 32-bit divide by zero would
        string nonzero = NL();
        T("xor %eax, %eax");
        for (int i = 0; i < words; i++) T($"or {dvs + i * 4}(%ebx), %eax");
        T("test %eax, %eax");
        T($"jnz {nonzero}");
        T("xor %edx, %edx");
        T("xor %ecx, %ecx");
        T("mov $1, %eax");
        T("div %ecx");                       // faults; does not return
        Lbl(nonzero);

        string top = NL(), restore = NL(), next = NL();
        T($"mov ${bits}, %ecx");
        Lbl(top);

        // one carry chain: shift the quotient left, and the bit that leaves it
        // arrives at the bottom of the remainder
        T($"shll $1, {quo}(%ebx)");
        for (int i = 1; i < words; i++) T($"rcll $1, {quo + i * 4}(%ebx)");
        for (int i = 0; i < words; i++) T($"rcll $1, {rem + i * 4}(%ebx)");

        // trial subtract: remainder -= divisor, then look at the borrow
        for (int i = 0; i < words; i++)
        {
            T($"mov {dvs + i * 4}(%ebx), %eax");
            T($"{(i == 0 ? "sub" : "sbb")} %eax, {rem + i * 4}(%ebx)");
        }
        T($"jc {restore}");
        T($"orl $1, {quo}(%ebx)");           // it fitted: record the bit
        T($"jmp {next}");

        Lbl(restore);                         // it did not: put the divisor back
        for (int i = 0; i < words; i++)
        {
            T($"mov {dvs + i * 4}(%ebx), %eax");
            T($"{(i == 0 ? "add" : "adc")} %eax, {rem + i * 4}(%ebx)");
        }

        Lbl(next);
        T("dec %ecx");
        T($"jnz {top}");

        // copy out the half that was asked for, restoring its sign
        int src  = wantRemainder ? rem  : quo;
        int flag = wantRemainder ? rneg : qneg;

        string done = NL();
        for (int i = 0; i < words; i++)
        {
            T($"mov {src + i * 4}(%ebx), %eax");
            T($"mov %eax, {i * 4}(%edx)");
        }
        if (signed)
        {
            T($"cmpl $0, {flag}(%ebx)");
            T($"je {done}");
            for (int i = 0; i < words; i++)
            {
                /* mov leaves the flags alone, which is what keeps the borrow
                   chain intact across the load. An xor here would clear the
                   carry and quietly corrupt every limb above the first. */
                T("mov $0, %eax");
                T($"{(i == 0 ? "sub" : "sbb")} {i * 4}(%edx), %eax");
                T($"mov %eax, {i * 4}(%edx)");
            }
            Lbl(done);
        }

        T($"add ${scratch}, %esp");
        T("pop %ebx");
    }

    /* Copy the wide value at [base] into scratch slot `dst` as a magnitude,
     * recording in `flag` (and optionally `alsoSet`) that it was negative.
     *
     * `xorFlag` is for the divisor: two negative operands make a positive
     * quotient, so the sign accumulates rather than overwrites.
     */
    private void EmitWideAbsTo(int dst, string baseReg, int words, int flag,
                               int alsoSet, bool xorFlag = false)
    {
        string neg = NL(), joined = NL();
        int top = (words - 1) * 4;

        T($"cmpl $0, {top}(%{baseReg})");
        T($"jl {neg}");
        for (int i = 0; i < words; i++)
        {
            T($"mov {i * 4}(%{baseReg}), %eax");
            T($"mov %eax, {dst + i * 4}(%ebx)");
        }
        T($"jmp {joined}");

        Lbl(neg);
        for (int i = 0; i < words; i++)
        {
            T("mov $0, %eax");
            T($"{(i == 0 ? "sub" : "sbb")} {i * 4}(%{baseReg}), %eax");
            T($"mov %eax, {dst + i * 4}(%ebx)");
        }
        if (xorFlag)
        {
            T($"movl $1, %eax");
            T($"xor %eax, {flag}(%ebx)");
        }
        else
        {
            T($"movl $1, {flag}(%ebx)");
            if (alsoSet >= 0) T($"movl $1, {alsoSet}(%ebx)");
        }
        Lbl(joined);
    }

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
        var ptypes = ParamTypesOf(c);
        var rt = TypeOf(c);
        // cdecl: push args right-to-left
        int argBytes = 0;
        for (int i = c.Args.Count - 1; i >= 0; i--)
        {
            var pt = i < ptypes.Count ? ptypes[i] : TypeOf(c.Args[i]);
            if (!IsAggregate(pt)) { EmitExpr(c.Args[i]); T("push %eax"); argBytes += 4; continue; }

            /* Copied onto the stack, top word first, so the value lands in
               ascending order - exactly what a C caller would have pushed. */
            int saved = _wideDepth;
            if (IsWide(pt)) EmitWideOperand(c.Args[i], pt); else EmitExpr(c.Args[i]);
            int size = Align4(SizeOf(pt));
            for (int o = size - 4; o >= 0; o -= 4) T($"pushl {o}(%eax)");
            argBytes += size;
            _wideDepth = saved;
        }
        bool sret = ReturnsViaPointer(rt) && _callSlots.ContainsKey(c);
        if (sret) { T($"lea {_callSlots[c]}(%ebp), %eax"); T("push %eax"); }
        /* Only a name that resolves to a FUNCTION is a direct call. A local or global
           holding a function pointer is also a NameExpr, and emitting `call <name>`
           for it calls a symbol that does not exist - it has to go through the value. */
        if (c.Callee is NameExpr nm
            && _ctx.Resolved.TryGetValue(nm, out var csym)
            && csym.Kind == SymKind.Function) T($"call {nm.Name}");
        else { EmitExpr(c.Callee); T("call *%eax"); }
        if (argBytes > 0) T($"add ${argBytes}, %esp");   // not the hidden pointer: the callee popped it

        if (sret) T($"lea {_callSlots[c]}(%ebp), %eax");
        else if (ReturnsInRegs64(rt) && _callSlots.TryGetValue(c, out var slot))
        {
            T($"mov %eax, {slot}(%ebp)");
            T($"mov %edx, {slot + 4}(%ebp)");
            T($"lea {slot}(%ebp), %eax");    // a wide value is its address, here as everywhere
        }
    }

    /* The declared parameter types of whatever is being called, so an
       argument is passed at the parameter's width - a literal handed to a u64
       parameter is pushed as 64 bits, not 32. */
    private List<TypeRef> ParamTypesOf(CallExpr c)
    {
        if (c.Callee is NameExpr nm && _ctx.Resolved.TryGetValue(nm, out var sym) && sym.Decl is FnDecl fn)
            return fn.Params.Select(p => p.Type).ToList();
        if (_ctx.Expand(TypeOf(c.Callee)) is FuncType ft) return ft.Params;
        return new List<TypeRef>();
    }

    /* Every call in a function body, for laying out result slots. Mirrors the
       statement walkers above; an expression that is never evaluated (the
       operand of sizeof) contributes nothing. */
    private IEnumerable<CallExpr> CallsIn(Stmt s)
    {
        switch (s)
        {
            case Block b: foreach (var x in b.Stmts) foreach (var c in CallsIn(x)) yield return c; break;
            case LetStmt l when l.Init != null: foreach (var c in CallsIn(l.Init)) yield return c; break;
            case AssignStmt a:
                foreach (var c in CallsIn(a.Target)) yield return c;
                foreach (var c in CallsIn(a.Value)) yield return c; break;
            case IfStmt i:
                foreach (var c in CallsIn(i.Cond)) yield return c;
                foreach (var c in CallsIn(i.Then)) yield return c;
                if (i.Else != null) foreach (var c in CallsIn(i.Else)) yield return c; break;
            case WhileStmt w:
                foreach (var c in CallsIn(w.Cond)) yield return c;
                foreach (var c in CallsIn(w.Body)) yield return c; break;
            case ReturnStmt r when r.Value != null: foreach (var c in CallsIn(r.Value)) yield return c; break;
            case ExprStmt e: foreach (var c in CallsIn(e.Expr)) yield return c; break;
            case DeferStmt d: foreach (var c in CallsIn(d.Body)) yield return c; break;
        }
    }
    private IEnumerable<CallExpr> CallsIn(Expr e)
    {
        switch (e)
        {
            case CallExpr c:
                yield return c;
                foreach (var x in CallsIn(c.Callee)) yield return x;
                foreach (var a in c.Args) foreach (var x in CallsIn(a)) yield return x; break;
            case BinaryExpr b:
                foreach (var x in CallsIn(b.Left)) yield return x;
                foreach (var x in CallsIn(b.Right)) yield return x; break;
            case UnaryExpr u: foreach (var x in CallsIn(u.Operand)) yield return x; break;
            case MemberExpr m: foreach (var x in CallsIn(m.Target)) yield return x; break;
            case IndexExpr ix:
                foreach (var x in CallsIn(ix.Target)) yield return x;
                foreach (var x in CallsIn(ix.Index)) yield return x; break;
            case CastExpr ce: foreach (var x in CallsIn(ce.Operand)) yield return x; break;
        }
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