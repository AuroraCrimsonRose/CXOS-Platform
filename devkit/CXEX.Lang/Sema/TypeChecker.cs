using System.Collections.Generic;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Sema;

/// <summary>
/// Pass 2: assign a type to every expression and check statements. v0.1 is
/// deliberately lenient on integer width (any int coerces to any int) so the
/// language is usable before a full numeric-conversion policy exists; pointers,
/// bools, structs, and arrays are checked strictly. Records each expr's type in
/// ctx.Types for CodeGen. The __syscall intrinsic is the one effect primitive.
/// </summary>
public sealed class TypeChecker
{
    private readonly SemaContext _ctx;
    private readonly DiagnosticBag _diag;
    private readonly ConstFold _fold;
    private TypeRef _curReturn = new PrimType(PrimKind.Void);

    private static readonly PrimType I32 = new(PrimKind.I32);
    private static readonly PrimType U32 = new(PrimKind.U32);
    private static readonly PrimType Bool = new(PrimKind.Bool);
    private static readonly PrimType Void = new(PrimKind.Void);

    public TypeChecker(SemaContext ctx, DiagnosticBag diag)
    { _ctx = ctx; _diag = diag; _fold = new ConstFold(ctx, diag); }

    public void Check(CompilationUnit unit)
    {
        foreach (var d in unit.Decls)
            switch (d)
            {
                case FnDecl f when f.Body != null:
                    _curReturn = f.Return; CheckBlock(f.Body); break;
                case ConstDecl c: _fold.TryEval(c.Value, out _); break;
                case GlobalDecl g when g.Init != null: _fold.TryEval(g.Init, out _); break;
            }
    }

    // ---- helpers ----
    private static bool IsInt(TypeRef t) => t is PrimType p && p.Kind is
        PrimKind.I8 or PrimKind.I16 or PrimKind.I32 or PrimKind.U8 or PrimKind.U16 or PrimKind.U32;
    private static bool IsBool(TypeRef t) => t is PrimType { Kind: PrimKind.Bool };
    private static bool IsPtr(TypeRef t) => t is PointerType;

    private static bool Same(TypeRef a, TypeRef b) => (a, b) switch
    {
        (PrimType x, PrimType y) => x.Kind == y.Kind,
        (PointerType x, PointerType y) => Same(x.Pointee, y.Pointee),
        (NamedType x, NamedType y) => x.Name == y.Name,
        (ArrayType x, ArrayType y) => x.Length == y.Length && Same(x.Element, y.Element),
        _ => false
    };

    // assignable: ints interchange (v0.1); else exact; ptr<-ptr exact pointee
    private bool Assignable(TypeRef to, TypeRef from)
        => (IsInt(to) && IsInt(from)) || Same(to, from);

    private TypeRef Set(Expr e, TypeRef t) { _ctx.Types[e] = t; return t; }

    private StructDecl? StructOf(TypeRef t)
        => t is NamedType n && _ctx.Structs.TryGetValue(n.Name, out var s) ? s : null;

    private static bool IsLValue(Expr e) => e is NameExpr or UnaryExpr { Op: UnOp.Deref } or IndexExpr or MemberExpr;

    // ---- statements ----
    private void CheckBlock(Block b) { foreach (var s in b.Stmts) CheckStmt(s); }

    private void CheckStmt(Stmt s)
    {
        switch (s)
        {
            case Block b: CheckBlock(b); break;

            case LetStmt l:
                {
                    var it = CheckExpr(l.Init);
                    if (l.Type != null)
                    {
                        if (!Assignable(l.Type, it))
                            _diag.Error($"cannot initialize '{l.Name}' of type {Show(l.Type)} from {Show(it)}", l.Span);
                        BindLocalType(l, l.Type);
                    }
                    else BindLocalType(l, it == Void ? I32 : it);
                    break;
                }
            case AssignStmt a:
                {
                    var tt = CheckExpr(a.Target);
                    var vt = CheckExpr(a.Value);
                    if (!IsLValue(a.Target)) _diag.Error("assignment target is not assignable", a.Target.Span);
                    else if (!Assignable(tt, vt)) _diag.Error($"cannot assign {Show(vt)} to {Show(tt)}", a.Span);
                    break;
                }
            case IfStmt i:
                Expect(CheckExpr(i.Cond), Bool, i.Cond.Span, "if condition");
                CheckBlock(i.Then); if (i.Else != null) CheckBlock(i.Else); break;
            case WhileStmt w:
                Expect(CheckExpr(w.Cond), Bool, w.Cond.Span, "while condition");
                CheckBlock(w.Body); break;
            case ReturnStmt r:
                if (r.Value == null) { if (!IsBool(_curReturn) && _curReturn is not PrimType { Kind: PrimKind.Void }) _diag.Error("return requires a value", r.Span); }
                else { var rt = CheckExpr(r.Value); if (!Assignable(_curReturn, rt)) _diag.Error($"return type {Show(rt)} does not match {Show(_curReturn)}", r.Span); }
                break;
            case ExprStmt e: CheckExpr(e.Expr); break;
        }
    }

    // LetStmt has no mutable field for the resolved local type; we re-bind via the
    // resolver's symbol by name lookup is unnecessary because CodeGen reads ctx.Types
    // for the init and the annotated/inferred type is recorded against the LetStmt's init.
    private readonly Dictionary<LetStmt, TypeRef> _localTypes = new();
    private void BindLocalType(LetStmt l, TypeRef t) => _localTypes[l] = t;
    public IReadOnlyDictionary<LetStmt, TypeRef> LocalTypes => _localTypes;

    // ---- expressions ----
    private TypeRef CheckExpr(Expr e)
    {
        switch (e)
        {
            case IntLit: return Set(e, I32);
            case BoolLit: return Set(e, Bool);

            case NameExpr n:
                if (n.Name == "__syscall") return Set(e, I32); // intrinsic callee placeholder
                return Set(e, _ctx.Resolved.TryGetValue(n, out var sym) ? sym.Type : Void);

            case CallExpr c: return Set(e, CheckCall(c));

            case MemberExpr m:
                {
                    var tt = CheckExpr(m.Target);
                    var sd = StructOf(tt is PointerType p ? p.Pointee : tt); // allow s.f and ps.f
                    if (sd == null) { _diag.Error($"'.{m.Field}' on non-struct {Show(tt)}", m.Span); return Set(e, Void); }
                    foreach (var f in sd.Fields) if (f.Name == m.Field) return Set(e, f.Type);
                    _diag.Error($"struct '{sd.Name}' has no field '{m.Field}'", m.Span); return Set(e, Void);
                }
            case IndexExpr ix:
                {
                    var tt = CheckExpr(ix.Target);
                    Expect(CheckExpr(ix.Index), I32, ix.Index.Span, "index");
                    TypeRef elem = tt switch { PointerType p => p.Pointee, ArrayType a => a.Element, _ => null! };
                    if (elem == null) { _diag.Error($"cannot index {Show(tt)}", ix.Span); return Set(e, Void); }
                    return Set(e, elem);
                }
            case UnaryExpr u:
                {
                    var ot = CheckExpr(u.Operand);
                    switch (u.Op)
                    {
                        case UnOp.Neg: if (!IsInt(ot)) _diag.Error("unary '-' needs an integer", u.Span); return Set(e, ot);
                        case UnOp.Not: if (!IsBool(ot)) _diag.Error("unary '!' needs a bool", u.Span); return Set(e, Bool);
                        case UnOp.Deref: if (ot is PointerType p) return Set(e, p.Pointee); _diag.Error("cannot dereference non-pointer", u.Span); return Set(e, Void);
                        case UnOp.AddrOf: return Set(e, new PointerType(ot));
                    }
                    return Set(e, Void);
                }
            case BinaryExpr b: return Set(e, CheckBinary(b));
            case CastExpr c: { CheckExpr(c.Operand); return Set(e, c.Target); } // v0.1: permissive

            default: return Set(e, Void);
        }
    }

    private TypeRef CheckBinary(BinaryExpr b)
    {
        var l = CheckExpr(b.Left); var r = CheckExpr(b.Right);
        switch (b.Op)
        {
            case BinOp.Add:
            case BinOp.Sub:
            case BinOp.Mul:
            case BinOp.Div:
            case BinOp.Mod:
                if (!IsInt(l) || !IsInt(r)) _diag.Error($"arithmetic on {Show(l)} and {Show(r)}", b.Span);
                return IsInt(l) ? l : I32;
            case BinOp.Eq:
            case BinOp.Ne:
                if (!(Assignable(l, r) || Assignable(r, l))) _diag.Error($"cannot compare {Show(l)} and {Show(r)}", b.Span);
                return Bool;
            case BinOp.Lt:
            case BinOp.Le:
            case BinOp.Gt:
            case BinOp.Ge:
                if (!IsInt(l) || !IsInt(r)) _diag.Error($"ordering on {Show(l)} and {Show(r)}", b.Span);
                return Bool;
            case BinOp.And:
            case BinOp.Or:
                if (!IsBool(l) || !IsBool(r)) _diag.Error("logical operator needs bools", b.Span);
                return Bool;
        }
        return I32;
    }

    private TypeRef CheckCall(CallExpr c)
    {
        // __syscall intrinsic: __syscall(n, a1..a5) -> i32
        if (c.Callee is NameExpr { Name: "__syscall" })
        {
            if (c.Args.Count < 1 || c.Args.Count > 6)
                _diag.Error("__syscall takes the number plus up to 5 args", c.Span);
            foreach (var a in c.Args) { var at = CheckExpr(a); if (!IsInt(at) && !IsPtr(at)) _diag.Error("__syscall args must be integer/pointer", a.Span); }
            return I32;
        }

        var ct = CheckExpr(c.Callee);
        if (c.Callee is NameExpr nm && _ctx.Resolved.TryGetValue(nm, out var sym) && sym.Decl is FnDecl fn)
        {
            if (c.Args.Count != fn.Params.Count)
                _diag.Error($"'{fn.Name}' expects {fn.Params.Count} args, got {c.Args.Count}", c.Span);
            for (int i = 0; i < c.Args.Count && i < fn.Params.Count; i++)
            {
                var at = CheckExpr(c.Args[i]);
                if (!Assignable(fn.Params[i].Type, at))
                    _diag.Error($"arg {i + 1} to '{fn.Name}': {Show(at)} not assignable to {Show(fn.Params[i].Type)}", c.Args[i].Span);
            }
            return fn.Return;
        }
        foreach (var a in c.Args) CheckExpr(a);
        _diag.Error("call target is not a function", c.Span);
        return Void;
    }

    private void Expect(TypeRef got, TypeRef want, SourceSpan span, string what)
    {
        if (!Assignable(want, got)) _diag.Error($"{what} must be {Show(want)}, got {Show(got)}", span);
    }

    private string Show(TypeRef t) => t switch
    {
        PrimType p => p.Kind.ToString().ToLowerInvariant(),
        PointerType p => "*" + Show(p.Pointee),
        ArrayType a => $"[{a.Length}]" + Show(a.Element),
        NamedType n => n.Name,
        _ => "?"
    };
}