using System;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Lexer;
using CXEX.Lang.Parsing;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Print the syntax tree of X source files, one node per line, as the parser
/// built it - before any name is resolved or type checked - followed by the
/// parser's diagnostics.
///
/// <para>The same format as <c>astdump</c>, the parser written in X (CXK os/xc),
/// so the two can be compared with a diff. Per file, a line <c>== path</c>; then
/// each node as <c>Kind line:col payload</c>, its children below it indented two
/// spaces more (a type has no position, so no <c>line:col</c>); then each error
/// as <c>error line:col message</c>.</para>
///
/// <para>Text - a name, a string - is printed in double quotes, with <c>"</c>,
/// <c>\</c> and control bytes as <c>\xHH</c>; a message escapes only the last
/// two. Children come in the order the node holds them, and a child that may
/// be absent is told apart by its kind: a type line is never an expression line.</para>
/// </summary>
public class AstCommand : Command<AstCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<FILES>")]
        [Description("X source files.")]
        public string[] Files { get; set; } = Array.Empty<string>();
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        using var stdout = Console.OpenStandardOutput();
        foreach (var path in settings.Files)
        {
            var name = Path.GetFileName(path);
            var toks = new Lexer(File.ReadAllText(path), name, new DiagnosticBag()).Tokenize();
            var diag = new DiagnosticBag();   // the parser's alone: `cxk tokens` covers the lexer's
            var unit = new Parser(toks, diag).Parse();
            var d = new AstDump();
            d.Sb.Append("== ");
            foreach (var b in Encoding.UTF8.GetBytes(path)) d.Sb.Append((char)b);
            d.Sb.Append('\n');
            d.Unit(unit);
            foreach (var e in diag.Items)
            {
                d.Sb.Append("error ").Append(e.Span.Line).Append(':').Append(e.Span.Col).Append(' ');
                Escape(d.Sb, e.Message, quote: false);
                d.Sb.Append('\n');
            }
            /* The builder holds bytes, one per char (see Escape), so it goes
               out as Latin-1: each char becomes exactly the byte it stands for. */
            var bytes = Encoding.Latin1.GetBytes(d.Sb.ToString());
            stdout.Write(bytes, 0, bytes.Length);
        }
        return 0;
    }

    /* Appends UTF-8 BYTES, one char each (0x80-0xFF included), so that what
       is printed is the bytes of the text - as the X side prints them. */
    internal static void Escape(StringBuilder sb, string s, bool quote)
    {
        if (quote) sb.Append('"');
        foreach (var b in Encoding.UTF8.GetBytes(s))
        {
            if (b < 0x20 || b == 0x7F || b == '\\' || (quote && b == '"')) sb.Append("\\x").Append(b.ToString("x2"));
            else sb.Append((char)b);
        }
        if (quote) sb.Append('"');
    }

    private sealed class AstDump
    {
        public readonly StringBuilder Sb = new();
        private int _depth;

        private StringBuilder Line(string kind, SourceSpan? at)
        {
            Sb.Append(' ', _depth * 2).Append(kind);
            if (at is SourceSpan s) Sb.Append(' ').Append(s.Line).Append(':').Append(s.Col);
            return Sb;
        }
        private void Text(string s) { Sb.Append(' '); Escape(Sb, s, quote: true); }
        private void End() => Sb.Append('\n');
        private void In() => _depth++;
        private void Out() => _depth--;

        public void Unit(CompilationUnit u)
        {
            Line("Unit", u.Span); End();
            In(); foreach (var d in u.Decls) Decl(d); Out();
        }

        private void Decl(Decl d)
        {
            switch (d)
            {
                case ImportDecl i: Line("Import", d.Span); Text(i.Path); break;
                case FnDecl f: Line("Fn", d.Span); Text(f.Name); if (f.Body == null) Sb.Append(" extern"); break;
                case StructDecl s: Line("Struct", d.Span); Text(s.Name); break;
                case TypeAliasDecl a: Line("Alias", d.Span); Text(a.Name); break;
                case GlobalDecl g: Line("Global", d.Span); Text(g.Name); break;
                case ConstDecl c: Line("Const", d.Span); Text(c.Name); break;
                case EnumDecl e: Line("Enum", d.Span); Text(e.Name); break;
            }
            End();
            In();
            foreach (var a in d.Attrs)
            {
                Line("Attr", a.Span); Text(a.Name); End();
                In();
                foreach (var arg in a.Args)
                {
                    Line("Arg", arg.Span); if (arg.Name != null) Text(arg.Name); End();
                    In(); Expr(arg.Value); Out();
                }
                Out();
            }
            switch (d)
            {
                case FnDecl f:
                    foreach (var p in f.Params) Param(p);
                    Type(f.Return);
                    if (f.Body != null) Stmt(f.Body);
                    break;
                case StructDecl s: foreach (var p in s.Fields) Param(p); break;
                case TypeAliasDecl a: Type(a.Target); break;
                case GlobalDecl g: Type(g.Type); if (g.Init != null) Expr(g.Init); break;
                case ConstDecl c: Type(c.Type); Expr(c.Value); break;
                case EnumDecl e:
                    if (e.Backing != null) Type(e.Backing);
                    foreach (var v in e.Variants)
                    {
                        Line("Variant", v.Span); Text(v.Name); if (v.Fields != null) Sb.Append(" sum"); End();
                        In();
                        if (v.Fields != null) foreach (var p in v.Fields) Param(p);
                        if (v.Value != null) Expr(v.Value);
                        Out();
                    }
                    break;
            }
            Out();
        }

        private void Param(Param p)
        {
            Line("Param", p.Span); Text(p.Name); End();
            In(); Type(p.Type); Out();
        }

        private void Type(TypeRef t)
        {
            switch (t)
            {
                case PrimType p: Line("Prim", null).Append(' ').Append(p.Kind.ToString().ToLowerInvariant()); End(); break;
                case PointerType p:
                    Line("Ptr", null);
                    if (p.Space != AddrSpace.Normal) Sb.Append(' ').Append(p.Space.ToString().ToLowerInvariant());
                    End();
                    In(); Type(p.Pointee); Out();
                    break;
                case ArrayType a: Line("Array", null).Append(' ').Append(a.Length); End(); In(); Type(a.Element); Out(); break;
                case NamedType n: Line("Named", null); Text(n.Name); End(); break;
                case FuncType f:
                    Line("FnType", null).Append(' ').Append(f.Params.Count); End();
                    In(); foreach (var p in f.Params) Type(p); Type(f.Return); Out();
                    break;
            }
        }

        private void Stmt(Stmt s)
        {
            switch (s)
            {
                case Block b:
                    Line("Block", s.Span); End();
                    In(); foreach (var x in b.Stmts) Stmt(x); Out();
                    break;
                case LetStmt l:
                    Line("Let", s.Span); Text(l.Name); End();
                    In(); if (l.Type != null) Type(l.Type); if (l.Init != null) Expr(l.Init); Out();
                    break;
                case AssignStmt a:
                    Line("Assign", s.Span); if (a.Compound) Sb.Append(" compound"); End();
                    In(); Expr(a.Target); Expr(a.Value); Out();
                    break;
                case IfStmt i:
                    Line("If", s.Span); End();
                    In(); Expr(i.Cond); Stmt(i.Then); if (i.Else != null) Stmt(i.Else); Out();
                    break;
                case WhileStmt w:
                    Line("While", s.Span); End();
                    In(); Expr(w.Cond); Stmt(w.Body); Out();
                    break;
                case ReturnStmt r:
                    Line("Return", s.Span); End();
                    In(); if (r.Value != null) Expr(r.Value); Out();
                    break;
                case BreakStmt: Line("Break", s.Span); End(); break;
                case ContinueStmt: Line("Continue", s.Span); End(); break;
                case ExprStmt e: Line("ExprStmt", s.Span); End(); In(); Expr(e.Expr); Out(); break;
                case DeferStmt d: Line("Defer", s.Span); End(); In(); Stmt(d.Body); Out(); break;
                case SwitchStmt w:
                    Line("Switch", s.Span); End();
                    In();
                    Expr(w.Subject);
                    foreach (var c in w.Cases)
                    {
                        Line("Case", c.Span); if (c.Bind != null) Text(c.Bind); End();
                        In(); foreach (var l in c.Labels) Expr(l); Stmt(c.Body); Out();
                    }
                    if (w.Else != null) Stmt(w.Else);
                    Out();
                    break;
            }
        }

        private static string Op(UnOp op) => op switch
        {
            UnOp.Neg => "neg", UnOp.Not => "not", UnOp.Deref => "deref", UnOp.AddrOf => "addr", _ => "bitnot",
        };

        private void Expr(Expr e)
        {
            switch (e)
            {
                case IntLit i:
                    Line("Int", e.Span).Append(' ').Append(i.Value.ToString());
                    if (i.IsChar) Sb.Append(" char");
                    else if (i.Suffix is PrimKind k) Sb.Append(' ').Append(k.ToString().ToLowerInvariant());
                    End();
                    break;
                case StrLit st: Line("Str", e.Span); Text(st.Value); End(); break;
                case BoolLit b: Line("Bool", e.Span).Append(b.Value ? " true" : " false"); End(); break;
                case NameExpr n: Line("Name", e.Span); Text(n.Name); End(); break;
                case CallExpr c:
                    Line("Call", e.Span); End();
                    In(); Expr(c.Callee); foreach (var a in c.Args) Expr(a); Out();
                    break;
                case MemberExpr m: Line("Member", e.Span); Text(m.Field); End(); In(); Expr(m.Target); Out(); break;
                case IndexExpr x: Line("Index", e.Span); End(); In(); Expr(x.Target); Expr(x.Index); Out(); break;
                case UnaryExpr u: Line("Unary", e.Span).Append(' ').Append(Op(u.Op)); End(); In(); Expr(u.Operand); Out(); break;
                case BinaryExpr b:
                    Line("Binary", e.Span).Append(' ').Append(b.Op.ToString().ToLowerInvariant()); End();
                    In(); Expr(b.Left); Expr(b.Right); Out();
                    break;
                case CastExpr c: Line("Cast", e.Span); End(); In(); Expr(c.Operand); Type(c.Target); Out(); break;
                case SizeofExpr z: Line("Sizeof", e.Span); End(); In(); Expr(z.Operand); Out(); break;
                case StructLit s:
                    Line("StructLit", e.Span); End();
                    In();
                    Expr(s.TypeName);
                    foreach (var f in s.Fields) { Line("Field", f.Span); Text(f.Name); End(); In(); Expr(f.Value); Out(); }
                    Out();
                    break;
                case ArrayLit a:
                    Line("ArrayLit", e.Span); End();
                    In(); foreach (var x in a.Items) Expr(x); Out();
                    break;
            }
        }
    }
}
