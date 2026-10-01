using System.Collections.Generic;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Sema;

public enum SymKind { Function, Struct, Global, Const, Param, Local, Enum }

public sealed class Symbol
{
    public string Name = "";
    public SymKind Kind;
    public TypeRef Type = new PrimType(PrimKind.Void); // value type (Function: return type)
    public Decl? Decl;                                  // FnDecl/StructDecl/GlobalDecl/ConstDecl
    public int FrameOffset;                             // filled by CodeGen for Param/Local
}

/// <summary>Lexical scope chain. Globals live at the root; functions push a child.</summary>
public sealed class Scope
{
    public readonly Scope? Parent;
    private readonly Dictionary<string, Symbol> _syms = new();
    public Scope(Scope? parent = null) { Parent = parent; }

    public bool Declare(Symbol s) => _syms.TryAdd(s.Name, s);
    public Symbol? Lookup(string n)
    {
        for (var s = this; s != null; s = s.Parent)
            if (s._syms.TryGetValue(n, out var sym)) return sym;
        return null;
    }
}

/// <summary>
/// Shared semantic state produced by the Resolver and consumed by the TypeChecker:
/// top-level symbols, struct decls by name, and per-NameExpr resolution.
/// </summary>
public sealed class SemaContext
{
    public readonly Scope Globals = new();
    public readonly Dictionary<string, StructDecl> Structs = new();
    public readonly Dictionary<Expr, Symbol> Resolved = new();   // NameExpr -> Symbol
    public readonly Dictionary<Expr, TypeRef> Types = new();     // filled by TypeChecker
    public readonly Dictionary<string, TypeRef> Aliases = new(); // type fx = i32;
    public readonly Dictionary<string, EnumDecl> Enums = new();
    /* The symbol each `let` declared, so the type checker can give it the type
       it infers. Without this a local declared as `let x = 5;` kept the
       placeholder type void, and every use of x was an error. */
    public readonly Dictionary<LetStmt, Symbol> LetSymbols = new();
    /* And each parameter's. With these two, a local's frame slot belongs to
       its DECLARATION: a name declared again in an inner block is a
       different variable, and the emitter can tell them apart. */
    public readonly Dictionary<Param, Symbol> ParamSymbols = new();
    /* `color.red` - a member expression naming a variant, resolved to its enum
       and index. Labels, values and constructors all go through this. */
    public readonly Dictionary<MemberExpr, (EnumDecl Enum, int Index)> EnumRefs = new();

    /* After type checking, the emitter sees a plain enum as its backing
       integer - which is all it is at run time. The checker must not: there an
       enum is its own type, and mixing it with integers needs `as`. */
    public bool ExpandEnums;

    public static string PayloadName(string en, string variant) => en + "." + variant;

    /// <summary>
    /// Expand a type alias to its target. Aliases are TRANSPARENT: `type fx = i32;`
    /// makes fx and i32 the same type, so fx values interoperate with integers.
    /// (Making them distinct would catch fx_mul(count, 5) but needs conversion
    /// rules; transparent is the v1 choice.) Depth-guarded against cycles.
    /// </summary>
    public TypeRef Expand(TypeRef t)
    {
        int guard = 0;
        while (t is NamedType n && Aliases.TryGetValue(n.Name, out var target) && guard++ < 16)
            t = target;
        if (ExpandEnums && t is NamedType en && Enums.TryGetValue(en.Name, out var ed) && !ed.IsSum)
            return ed.Backing != null ? Expand(ed.Backing) : new PrimType(PrimKind.U32);
        return t;
    }

    /* Size in bytes, the same rule as the emitter's - needed here only to lay
       out sum types, whose size is the tag plus the largest payload.

       A type that contains itself by value - directly, or through other
       types - has no size. Sizing it recursed until the compiler died of a
       stack overflow, so the error meant for it could never be reported. A
       type met again while it is being sized now counts 0 and is remembered
       in SizeCycle, for the caller to report. */
    private readonly HashSet<string> _sizing = new();
    public string? SizeCycle;

    /* Saturating: a size too large to count in an int comes back as
       int.MaxValue rather than wrapping to something small or negative, so the
       type checker can refuse it (see CheckSize) before the emitter lays out a
       frame with it. `[0x7FFFFFFF]u32` used to come out at -4 bytes. */
    public int SizeOf(TypeRef t0)
    {
        var t = Expand(t0);
        switch (t)
        {
            case PrimType p: return PrimWidth.Bytes(p.Kind);
            case ArrayType a: return (int)Math.Min((long)SizeOf(a.Element) * a.Length, int.MaxValue);
            case NamedType n when Structs.ContainsKey(n.Name) || Enums.ContainsKey(n.Name):
                if (!_sizing.Add(n.Name)) { SizeCycle ??= n.Name; return 0; }
                try { return SizeOfNamed(n.Name); }
                finally { _sizing.Remove(n.Name); }
            default: return 4;
        }
    }

    private int SizeOfNamed(string name)
    {
        if (Structs.TryGetValue(name, out var s))
        { long o = 0; foreach (var f in s.Fields) o += ((long)SizeOf(f.Type) + 3) & ~3L; return (int)Math.Min(o, int.MaxValue); }
        var e = Enums[name];
        if (!e.IsSum) return e.Backing != null ? SizeOf(e.Backing) : 4;
        int max = 0;
        foreach (var v in e.Variants)
            if (v.Fields != null)
            {
                long o = 0; foreach (var f in v.Fields) o += ((long)SizeOf(f.Type) + 3) & ~3L;
                if (o > max) max = (int)Math.Min(o, int.MaxValue - 4);
            }
        return 4 + max;
    }
}

/// <summary>
/// Pass 1: collect top-level declarations, then resolve every name to a symbol,
/// tracking lexical scopes for params/locals. Reports duplicates + undefined names.
/// </summary>
public sealed class Resolver
{
    private readonly DiagnosticBag _diag;
    private readonly SemaContext _ctx = new();

    public Resolver(DiagnosticBag diag) { _diag = diag; }

    public SemaContext Resolve(CompilationUnit unit)
    {
        foreach (var d in unit.Decls) DeclareTop(d);   // 1a: forward-visible top-level symbols
        LayOutSums();
        foreach (var d in unit.Decls) ResolveDecl(d);  // 1b: bodies
        return _ctx;
    }

    private void DeclareTop(Decl d)
    {
        // imports are resolved by the driver before sema; they declare nothing
        if (d is ImportDecl) return;

        /* An enum's name is a symbol so `color.red` resolves through it. A sum
           type also gets one struct per variant - its fields, reached through a
           switch binding - and is itself laid out as a struct (below). */
        if (d is EnumDecl ed)
        {
            _ctx.Enums[ed.Name] = ed;
            if (!_ctx.Globals.Declare(new Symbol { Name = ed.Name, Kind = SymKind.Enum, Type = new NamedType(ed.Name), Decl = ed }))
                _diag.Error($"duplicate top-level declaration '{ed.Name}'", d.Span);
            var names = new HashSet<string>();
            foreach (var v in ed.Variants)
            {
                if (!names.Add(v.Name)) _diag.Error($"'{ed.Name}' has two variants named '{v.Name}'", v.Span);
                if (v.Fields != null)
                    _ctx.Structs[SemaContext.PayloadName(ed.Name, v.Name)] =
                        new StructDecl(SemaContext.PayloadName(ed.Name, v.Name), v.Fields) { Span = v.Span };
            }
            return;
        }

        // type aliases go in their own table, not the value scope
        if (d is TypeAliasDecl ta)
        {
            if (!_ctx.Aliases.TryAdd(ta.Name, ta.Target))
                _diag.Error($"duplicate type alias '{ta.Name}'", d.Span);
            return;
        }

        // a function's symbol type is its full signature, so a bare function name
        // used as a value (a function pointer) types correctly
        if (d is FnDecl fd)
        {
            var ps = new List<TypeRef>();
            foreach (var pm in fd.Params) ps.Add(pm.Type);
            var fsym = new Symbol { Name = fd.Name, Kind = SymKind.Function,
                                    Type = new FuncType(ps, fd.Return), Decl = fd };
            if (!_ctx.Globals.Declare(fsym))
                _diag.Error($"duplicate top-level declaration '{fd.Name}'", d.Span);
            return;
        }

        Symbol sym = d switch
        {
            FnDecl f => new Symbol { Name = f.Name, Kind = SymKind.Function, Type = f.Return, Decl = f },
            StructDecl s => new Symbol { Name = s.Name, Kind = SymKind.Struct, Type = new NamedType(s.Name), Decl = s },
            GlobalDecl g => new Symbol { Name = g.Name, Kind = SymKind.Global, Type = g.Type, Decl = g },
            ConstDecl c => new Symbol { Name = c.Name, Kind = SymKind.Const, Type = c.Type, Decl = c },
            _ => new Symbol { Name = "?" }
        };
        if (d is StructDecl sd) _ctx.Structs[sd.Name] = sd;
        if (!_ctx.Globals.Declare(sym))
            _diag.Error($"duplicate top-level declaration '{sym.Name}'", d.Span);
    }

    /* A sum type is a struct the program cannot name the fields of: a u32 tag
       and room for the largest payload. `$` cannot appear in an identifier, so
       the only way in is a switch - and a sum value can be copied, passed and
       returned exactly like any other struct. */
    private void LayOutSums()
    {
        foreach (var ed in _ctx.Enums.Values)
        {
            if (!ed.IsSum) continue;
            _ctx.SizeCycle = null;
            int body = _ctx.SizeOf(new NamedType(ed.Name)) - 4;
            if (_ctx.SizeCycle != null)
            {
                _diag.Error($"'{ed.Name}' contains '{_ctx.SizeCycle}' by value inside itself, so it has no size; use a pointer (*{_ctx.SizeCycle})", ed.Span);
                body = 0;
            }
            _ctx.Structs[ed.Name] = new StructDecl(ed.Name, new List<Param>
            {
                new("$tag", new PrimType(PrimKind.U32)),
                new("$body", new ArrayType(new PrimType(PrimKind.U8), body)),
            }) { Span = ed.Span };
        }
    }

    private void ResolveDecl(Decl d)
    {
        switch (d)
        {
            case FnDecl f when f.Body != null:
                var fnScope = new Scope(_ctx.Globals);
                foreach (var p in f.Params)
                {
                    var ps = new Symbol { Name = p.Name, Kind = SymKind.Param, Type = p.Type };
                    _ctx.ParamSymbols[p] = ps;
                    if (!fnScope.Declare(ps))
                        _diag.Error($"duplicate parameter '{p.Name}'", p.Span);
                }
                ResolveBlock(f.Body, fnScope);
                break;
            case GlobalDecl g when g.Init != null: ResolveExpr(g.Init, _ctx.Globals); break;
            case ConstDecl c: ResolveExpr(c.Value, _ctx.Globals); break;
        }
    }

    private void ResolveBlock(Block b, Scope parent)
    {
        var scope = new Scope(parent);
        foreach (var s in b.Stmts) ResolveStmt(s, scope);
    }

    private void ResolveStmt(Stmt s, Scope scope)
    {
        switch (s)
        {
            case Block b: ResolveBlock(b, scope); break;
            case LetStmt l:
                {
                    if (l.Init != null) ResolveExpr(l.Init, scope);   // `let x: T;` has no init
                    var ls = new Symbol { Name = l.Name, Kind = SymKind.Local, Type = l.Type ?? new PrimType(PrimKind.Void) };
                    _ctx.LetSymbols[l] = ls;
                    if (!scope.Declare(ls)) _diag.Error($"duplicate local '{l.Name}'", l.Span);
                    break;
                }
            case AssignStmt a: ResolveExpr(a.Target, scope); ResolveExpr(a.Value, scope); break;
            case IfStmt i:
                ResolveExpr(i.Cond, scope); ResolveBlock(i.Then, scope);
                if (i.Else != null) ResolveBlock(i.Else, scope);
                break;
            case WhileStmt w: ResolveExpr(w.Cond, scope); ResolveBlock(w.Body, scope); break;
            case ReturnStmt r: if (r.Value != null) ResolveExpr(r.Value, scope); break;
            case ExprStmt e: ResolveExpr(e.Expr, scope); break;
            /* Resolved in the scope at the point of the `defer`, not at the
               end of the block where it runs. So a deferred body sees what was
               declared before it and nothing declared after - the same rule as
               every other statement, even though the code runs later. */
            case DeferStmt d: ResolveStmt(d.Body, scope); break;
            case SwitchStmt sw:
                ResolveExpr(sw.Subject, scope);
                foreach (var c in sw.Cases)
                {
                    foreach (var l in c.Labels) ResolveExpr(l, scope);
                    var cs = new Scope(scope);
                    if (c.BindLet != null)
                    {
                        var bs = new Symbol { Name = c.BindLet.Name, Kind = SymKind.Local };
                        _ctx.LetSymbols[c.BindLet] = bs;
                        cs.Declare(bs);
                    }
                    ResolveBlock(c.Body, cs);
                }
                if (sw.Else != null) ResolveBlock(sw.Else, scope);
                break;
        }
    }

    private void ResolveExpr(Expr e, Scope scope)
    {
        switch (e)
        {
            case NameExpr n:
                if (n.Name == "__syscall") break;           // intrinsic, handled in TypeChecker
                var sym = scope.Lookup(n.Name);
                if (sym == null) _diag.Error($"undefined name '{n.Name}'", n.Span);
                else _ctx.Resolved[n] = sym;
                break;
            case CallExpr c: ResolveExpr(c.Callee, scope); foreach (var a in c.Args) ResolveExpr(a, scope); break;
            case MemberExpr m:
                /* `Enum.variant` is not a field access: record which variant. */
                if (m.Target is NameExpr en && scope.Lookup(en.Name) is { Kind: SymKind.Enum, Decl: EnumDecl ed })
                {
                    _ctx.Resolved[en] = scope.Lookup(en.Name)!;
                    int i = ed.IndexOf(m.Field);
                    if (i < 0) _diag.Error($"'{ed.Name}' has no variant '{m.Field}'", m.Span);
                    else _ctx.EnumRefs[m] = (ed, i);
                    break;
                }
                ResolveExpr(m.Target, scope);
                break;
            case IndexExpr ix: ResolveExpr(ix.Target, scope); ResolveExpr(ix.Index, scope); break;
            case UnaryExpr u: ResolveExpr(u.Operand, scope); break;
            case SizeofExpr sz: ResolveExpr(sz.Operand, scope); break;
            case BinaryExpr b: ResolveExpr(b.Left, scope); ResolveExpr(b.Right, scope); break;
            case CastExpr ca: ResolveExpr(ca.Operand, scope); break;
            case StructLit sl:
                ResolveExpr(sl.TypeName, scope);
                foreach (var f in sl.Fields) ResolveExpr(f.Value, scope);
                break;
            case ArrayLit al: foreach (var i in al.Items) ResolveExpr(i, scope); break;
        }
    }
}