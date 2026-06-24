using System.Collections.Generic;
using System.Text;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Sema;

namespace CXEX.Lang.CodeGen;

/// <summary>
/// X core v0.1 backend: lowers the typed AST to x86-32 GAS (AT&T) assembly text,
/// which the build assembles+links (i686-elf) to an ELF, then ElfParser/CXEXWriter
/// package into a .xcex/.xoex. Straightforward stack-machine codegen, cdecl ABI:
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
    private int _label;

    // current function frame: name -> (ebp offset, type)
    private Dictionary<string, (int off, TypeRef ty)> _frame = new();

    public X86Emitter(SemaContext ctx, IReadOnlyDictionary<LetStmt, TypeRef> localTypes, DiagnosticBag diag)
    { _ctx = ctx; _localTypes = localTypes; _diag = diag; }

    private string NL() => $".L{_label++}";
    private void T(string s) => _text.AppendLine("    " + s);
    private void Lbl(string l) => _text.AppendLine(l + ":");

    public string Emit(CompilationUnit unit)
    {
        _text.AppendLine(".text");
        _text.AppendLine(".globl _start");
        _data.AppendLine(".data");
        foreach (var d in unit.Decls)
        {
            if (d is FnDecl f && f.Body != null) EmitFn(f);
            else if (d is GlobalDecl g) EmitGlobal(g);
        }
        return _text + "\n" + _data;
    }

    // ---- type sizes / struct layout (v0.1: every field 4-aligned, matches ABI structs) ----
    private int SizeOf(TypeRef t) => t switch
    {
        PrimType p => p.Kind switch
        {
            PrimKind.I8 or PrimKind.U8 or PrimKind.Bool => 1,
            PrimKind.I16 or PrimKind.U16 => 2,
            _ => 4
        },
        PointerType => 4,
        ArrayType a => Align4(SizeOf(a.Element)) * a.Length,
        NamedType n when _ctx.Structs.TryGetValue(n.Name, out var s) => StructSize(s),
        _ => 4
    };
    private static int Align4(int n) => (n + 3) & ~3;
    private int StructSize(StructDecl s) { int o = 0; foreach (var f in s.Fields) o += Align4(SizeOf(f.Type)); return o; }
    private int FieldOffset(StructDecl s, string field)
    { int o = 0; foreach (var f in s.Fields) { if (f.Name == field) return o; o += Align4(SizeOf(f.Type)); } return 0; }
    private StructDecl? StructOf(TypeRef t) => t is NamedType n && _ctx.Structs.TryGetValue(n.Name, out var s) ? s : null;

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

    private void EmitGlobal(GlobalDecl g)
    {
        int size = Align4(SizeOf(g.Type));
        ulong init = 0;
        if (g.Init != null) new ConstFold(_ctx, _diag).TryEval(g.Init, out init);
        _data.AppendLine($"{g.Name}:");
        if (size == 4) _data.AppendLine($"    .long {init}");
        else { _data.AppendLine($"    .long {init}"); for (int i = 4; i < size; i += 4) _data.AppendLine("    .long 0"); }
    }

    // ---- statements ----
    private void EmitBlock(Block b) { foreach (var s in b.Stmts) EmitStmt(s); }

    private void EmitStmt(Stmt s)
    {
        switch (s)
        {
            case Block b: EmitBlock(b); break;
            case LetStmt l: EmitExpr(l.Init); StoreToVar(l.Name); break;   // init -> eax -> slot
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
                    EmitBlock(w.Body); T($"jmp {top}"); Lbl(end); break;
                }
        }
    }

    private string CurFnRet => _curFn + "$ret";
    private string _curFn = "";

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
        T("mov %ecx, (%eax)");
    }

    // ---- expressions: result in eax ----
    private void EmitExpr(Expr e)
    {
        switch (e)
        {
            case IntLit i: T($"mov ${i.Value}, %eax"); break;
            case BoolLit b: T($"mov ${(b.Value ? 1 : 0)}, %eax"); break;
            case NameExpr n: EmitName(n); break;
            case BinaryExpr b: EmitBinary(b); break;
            case UnaryExpr u: EmitUnary(u); break;
            case CallExpr c: EmitCall(c); break;
            case CastExpr c: EmitExpr(c.Operand); break;          // v0.1: reinterpret, no convert
            case MemberExpr or IndexExpr: EmitAddr(e); T("mov (%eax), %eax"); break; // load value at field/elem
            default: T("xor %eax, %eax"); break;
        }
    }

    private void EmitName(NameExpr n)
    {
        if (!_ctx.Resolved.TryGetValue(n, out var sym)) { T("xor %eax, %eax"); return; }
        if (sym.Kind == SymKind.Const && sym.Decl is ConstDecl cd && new ConstFold(_ctx, _diag).TryEval(cd.Value, out var v))
        { T($"mov ${v}, %eax"); return; }
        if (sym.Kind == SymKind.Global) { T($"mov {n.Name}, %eax"); return; }
        if (_frame.TryGetValue(n.Name, out var slot))
        {
            // struct/array names yield their address (decay); scalars load value
            if (slot.ty is NamedType or ArrayType) T($"lea {slot.off}(%ebp), %eax");
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
                    int es = tt switch { PointerType p => SizeOf(p.Pointee), ArrayType a => Align4(SizeOf(a.Element)), _ => 4 };
                    if (tt is ArrayType) EmitAddr(ix.Target); else EmitExpr(ix.Target);
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
            case UnOp.Deref: EmitExpr(u.Operand); T("mov (%eax), %eax"); break;
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
            case BinOp.Div: T("cdq"); T("idiv %ecx"); break;
            case BinOp.Mod: T("cdq"); T("idiv %ecx"); T("mov %edx, %eax"); break;
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
        if (c.Callee is NameExpr nm) T($"call {nm.Name}");
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