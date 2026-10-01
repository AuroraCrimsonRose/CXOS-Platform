using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Ast;
using CXEX.Lang.Sema;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Print what the front end concludes about X programs: every diagnostic, in
/// order, and - when type checking ran - the type of every expression and of
/// every local.
///
/// <para>The same format as <c>semadump</c>, the type checker written in X (CXK
/// os/xc), so the two can be compared with a diff. Each SOURCE is a whole
/// program, analysed exactly as <c>cxk compile</c> would: the prelude, the
/// file, everything it imports. Per program:</para>
/// <code>
/// == path
/// error file:line:col message        every diagnostic, in the order reported
/// checked yes|no                     whether type checking ran
/// Kind file:line:col type            every expression, in tree order, with its type (- if none)
/// Let file:line:col "name" type      every local, with the type it was given
/// </code>
/// <para>A type is written out in full - <c>fn(*u8, u32) -> bool</c> - and a
/// message escapes <c>\</c> and control bytes as <c>\xHH</c>.</para>
/// </summary>
public class SemaCommand : Command<SemaCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCES>")]
        [Description("X programs: each is analysed on its own, with what it imports.")]
        public string[] Sources { get; set; } = Array.Empty<string>();

        [CommandOption("-I|--include <DIR>")]
        [Description("Extra directory to search for imports (repeatable)")]
        public string[] IncludeDirs { get; set; } = Array.Empty<string>();

        [CommandOption("--no-prelude")]
        [Description("do not prepend the generated abi.x prelude")]
        public bool NoPrelude { get; set; }
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken cancellationToken)
    {
        using var stdout = Console.OpenStandardOutput();
        foreach (var path in s.Sources)
        {
            var fe = Frontend.Analyze(path, s.NoPrelude, s.IncludeDirs);
            var d = new Dump(fe);
            d.Bytes("== " + path); d.Sb.Append('\n');
            foreach (var e in fe.Diag.Items)
            {
                d.Sb.Append("error ").Append(e.Span.File).Append(':').Append(e.Span.Line).Append(':').Append(e.Span.Col).Append(' ');
                AstCommand.Escape(d.Sb, e.Message, quote: false);
                d.Sb.Append('\n');
            }
            d.Sb.Append(fe.Checker != null ? "checked yes\n" : "checked no\n");
            if (fe.Checker != null) foreach (var decl in fe.Unit.Decls) d.Decl(decl);
            var bytes = Encoding.Latin1.GetBytes(d.Sb.ToString());   // one char per byte; see AstCommand.Escape
            stdout.Write(bytes, 0, bytes.Length);
        }
        return 0;
    }

    /// <summary>A type, written out in full.</summary>
    public static string Show(TypeRef t) => t switch
    {
        PrimType p => p.Kind.ToString().ToLowerInvariant(),
        PointerType p => "*" + p.Space switch
        {
            AddrSpace.User => "user ", AddrSpace.Phys => "phys ", AddrSpace.Dma => "dma ", _ => "",
        } + Show(p.Pointee),
        ArrayType a => $"[{a.Length}]" + Show(a.Element),
        NamedType n => n.Name,
        FuncType f => "fn(" + string.Join(", ", f.Params.ConvertAll(Show)) + ") -> " + Show(f.Return),
        _ => "?",
    };

    private sealed class Dump
    {
        public readonly StringBuilder Sb = new();
        private readonly Frontend _fe;
        public Dump(Frontend fe) { _fe = fe; }

        public void Bytes(string s) { foreach (var b in Encoding.UTF8.GetBytes(s)) Sb.Append((char)b); }

        private void At(string kind, Node n)
        {
            Sb.Append(kind).Append(' ');
            Bytes(n.Span.File);
            Sb.Append(':').Append(n.Span.Line).Append(':').Append(n.Span.Col);
        }

        private void Type(TypeRef? t)
        {
            Sb.Append(' ');
            if (t == null) Sb.Append('-'); else Bytes(Show(t));
            Sb.Append('\n');
        }

        private void Local(LetStmt l)
        {
            At("Let", l);
            Sb.Append(' ');
            AstCommand.Escape(Sb, l.Name, quote: true);
            Type(_fe.Checker!.LocalTypes.TryGetValue(l, out var t) ? t : null);
        }

        public void Decl(Decl d)
        {
            foreach (var a in d.Attrs) foreach (var arg in a.Args) Expr(arg.Value);
            switch (d)
            {
                case FnDecl f: if (f.Body != null) Stmt(f.Body); break;
                case GlobalDecl g: if (g.Init != null) Expr(g.Init); break;
                case ConstDecl c: Expr(c.Value); break;
                case EnumDecl e: foreach (var v in e.Variants) if (v.Value != null) Expr(v.Value); break;
            }
        }

        private void Stmt(Stmt s)
        {
            switch (s)
            {
                case Block b: foreach (var x in b.Stmts) Stmt(x); break;
                case LetStmt l: if (l.Init != null) Expr(l.Init); Local(l); break;
                case AssignStmt a: Expr(a.Target); Expr(a.Value); break;
                case IfStmt i: Expr(i.Cond); Stmt(i.Then); if (i.Else != null) Stmt(i.Else); break;
                case WhileStmt w: Expr(w.Cond); Stmt(w.Body); break;
                case ReturnStmt r: if (r.Value != null) Expr(r.Value); break;
                case ExprStmt e: Expr(e.Expr); break;
                case DeferStmt d: Stmt(d.Body); break;
                case SwitchStmt w:
                    Expr(w.Subject);
                    foreach (var c in w.Cases)
                    {
                        foreach (var l in c.Labels) Expr(l);
                        if (c.BindLet != null) Local(c.BindLet);
                        Stmt(c.Body);
                    }
                    if (w.Else != null) Stmt(w.Else);
                    break;
            }
        }

        private void Expr(Expr e)
        {
            At(e.GetType().Name, e);
            Type(_fe.Ctx.Types.TryGetValue(e, out var t) ? t : null);
            switch (e)
            {
                case CallExpr c: Expr(c.Callee); foreach (var a in c.Args) Expr(a); break;
                case MemberExpr m: Expr(m.Target); break;
                case IndexExpr x: Expr(x.Target); Expr(x.Index); break;
                case UnaryExpr u: Expr(u.Operand); break;
                case BinaryExpr b: Expr(b.Left); Expr(b.Right); break;
                case CastExpr c: Expr(c.Operand); break;
                case SizeofExpr z: Expr(z.Operand); break;
                case StructLit s: Expr(s.TypeName); foreach (var f in s.Fields) Expr(f.Value); break;
                case ArrayLit a: foreach (var x in a.Items) Expr(x); break;
            }
        }
    }
}
