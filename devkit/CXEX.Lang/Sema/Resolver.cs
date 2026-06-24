using System.Collections.Generic;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Sema;

public enum SymKind { Function, Struct, Global, Const, Param, Local }

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
        foreach (var d in unit.Decls) ResolveDecl(d);  // 1b: bodies
        return _ctx;
    }

    private void DeclareTop(Decl d)
    {
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

    private void ResolveDecl(Decl d)
    {
        switch (d)
        {
            case FnDecl f when f.Body != null:
                var fnScope = new Scope(_ctx.Globals);
                foreach (var p in f.Params)
                    if (!fnScope.Declare(new Symbol { Name = p.Name, Kind = SymKind.Param, Type = p.Type }))
                        _diag.Error($"duplicate parameter '{p.Name}'", p.Span);
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
                ResolveExpr(l.Init, scope);
                if (!scope.Declare(new Symbol { Name = l.Name, Kind = SymKind.Local, Type = l.Type ?? new PrimType(PrimKind.Void) }))
                    _diag.Error($"duplicate local '{l.Name}'", l.Span);
                break;
            case AssignStmt a: ResolveExpr(a.Target, scope); ResolveExpr(a.Value, scope); break;
            case IfStmt i:
                ResolveExpr(i.Cond, scope); ResolveBlock(i.Then, scope);
                if (i.Else != null) ResolveBlock(i.Else, scope);
                break;
            case WhileStmt w: ResolveExpr(w.Cond, scope); ResolveBlock(w.Body, scope); break;
            case ReturnStmt r: if (r.Value != null) ResolveExpr(r.Value, scope); break;
            case ExprStmt e: ResolveExpr(e.Expr, scope); break;
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
            case MemberExpr m: ResolveExpr(m.Target, scope); break;
            case IndexExpr ix: ResolveExpr(ix.Target, scope); ResolveExpr(ix.Index, scope); break;
            case UnaryExpr u: ResolveExpr(u.Operand, scope); break;
            case BinaryExpr b: ResolveExpr(b.Left, scope); ResolveExpr(b.Right, scope); break;
            case CastExpr ca: ResolveExpr(ca.Operand, scope); break;
        }
    }
}