using System.Collections.Generic;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Lexer;

namespace CXEX.Lang.Parsing;

/// <summary>
/// Recursive-descent parser for X core v0.1. Expressions use precedence climbing.
/// On error it reports a diagnostic and synchronizes to the next ';' or '}' / top-
/// level keyword, so one bad construct doesn't cascade. Returns a CompilationUnit
/// regardless; callers check diag.HasErrors before proceeding to Sema.
/// </summary>
public sealed class Parser
{
    private readonly List<Token> _toks;
    private readonly DiagnosticBag _diag;
    private int _i;

    public Parser(List<Token> tokens, DiagnosticBag diag) { _toks = tokens; _diag = diag; }

    private Token Cur => _toks[_i];
    private Token Peek(int n = 1) => _toks[System.Math.Min(_i + n, _toks.Count - 1)];
    private bool At(TokenKind k) => Cur.Kind == k;
    private Token Advance() => _toks[_i < _toks.Count - 1 ? _i++ : _i];

    private bool Match(TokenKind k) { if (At(k)) { Advance(); return true; } return false; }

    private Token Expect(TokenKind k, string what)
    {
        if (At(k)) return Advance();
        _diag.Error($"expected {what}, found '{Cur.Text}'", Cur.Span);
        return Cur; // don't advance; let sync handle it
    }

    private SourceSpan To(SourceSpan start) =>
        start with { End = _toks[System.Math.Max(_i - 1, 0)].Span.End };

    // ---- program ----
    public CompilationUnit Parse()
    {
        var start = Cur.Span;
        var decls = new List<Decl>();
        while (!At(TokenKind.Eof))
        {
            int before = _i;
            var d = ParseDecl();
            if (d != null) decls.Add(d);
            if (_i == before) SyncTopLevel(); // no progress => force advance
        }
        return new CompilationUnit(decls) { Span = To(start) };
    }

    private void SyncTopLevel()
    {
        while (!At(TokenKind.Eof) && !IsDeclStart(Cur.Kind)) Advance();
    }

    private static bool IsDeclStart(TokenKind k) =>
        k is TokenKind.Fn or TokenKind.Struct or TokenKind.Global
          or TokenKind.Const or TokenKind.Extern;

    // ---- declarations ----
    private Decl? ParseDecl()
    {
        var start = Cur.Span;
        switch (Cur.Kind)
        {
            case TokenKind.Fn: return ParseFn(start, isExtern: false);
            case TokenKind.Extern: Advance(); return ParseFn(start, isExtern: true);  // ParseFn consumes 'fn'
            case TokenKind.Struct: return ParseStruct(start);
            case TokenKind.Global: return ParseGlobal(start);
            case TokenKind.Const: return ParseConst(start);
            default:
                _diag.Error($"expected a declaration, found '{Cur.Text}'", Cur.Span);
                return null;
        }
    }

    private FnDecl ParseFn(SourceSpan start, bool isExtern)
    {
        Advance(); // consume 'fn' (reached after 'fn' or after 'extern')
        var name = Expect(TokenKind.Identifier, "function name").Text;
        Expect(TokenKind.LParen, "'('");
        var ps = new List<Param>();
        if (!At(TokenKind.RParen))
        {
            do { ps.Add(ParseParam()); } while (Match(TokenKind.Comma));
        }
        Expect(TokenKind.RParen, "')'");
        TypeRef ret = new PrimType(PrimKind.Void);
        if (Match(TokenKind.Arrow)) ret = ParseType();
        Block? body = null;
        if (isExtern) Expect(TokenKind.Semicolon, "';'");
        else body = ParseBlock();
        return new FnDecl(name, ps, ret, body) { Span = To(start) };
    }

    private Param ParseParam()
    {
        var start = Cur.Span;
        var name = Expect(TokenKind.Identifier, "parameter name").Text;
        Expect(TokenKind.Colon, "':'");
        var ty = ParseType();
        return new Param(name, ty) { Span = To(start) };
    }

    private StructDecl ParseStruct(SourceSpan start)
    {
        Advance(); // 'struct'
        var name = Expect(TokenKind.Identifier, "struct name").Text;
        Expect(TokenKind.LBrace, "'{'");
        var fields = new List<Param>();
        if (!At(TokenKind.RBrace))
        {
            do { if (At(TokenKind.RBrace)) break; fields.Add(ParseParam()); } while (Match(TokenKind.Comma));
        }
        Expect(TokenKind.RBrace, "'}'");
        return new StructDecl(name, fields) { Span = To(start) };
    }

    private GlobalDecl ParseGlobal(SourceSpan start)
    {
        Advance(); // 'global'
        var name = Expect(TokenKind.Identifier, "global name").Text;
        Expect(TokenKind.Colon, "':'");
        var ty = ParseType();
        Expr? init = null;
        if (Match(TokenKind.Assign)) init = ParseExpr();
        Expect(TokenKind.Semicolon, "';'");
        return new GlobalDecl(name, ty, init) { Span = To(start) };
    }

    private ConstDecl ParseConst(SourceSpan start)
    {
        Advance(); // 'const'
        var name = Expect(TokenKind.Identifier, "const name").Text;
        Expect(TokenKind.Colon, "':'");
        var ty = ParseType();
        Expect(TokenKind.Assign, "'='");
        var val = ParseExpr();
        Expect(TokenKind.Semicolon, "';'");
        return new ConstDecl(name, ty, val) { Span = To(start) };
    }

    // ---- types ----
    private TypeRef ParseType()
    {
        var start = Cur.Span;
        if (Match(TokenKind.Star)) return new PointerType(ParseType());
        if (Match(TokenKind.LBracket))
        {
            ulong n = 0;
            if (At(TokenKind.IntLiteral)) n = Advance().Value;
            else _diag.Error("expected array length", Cur.Span);
            Expect(TokenKind.RBracket, "']'");
            return new ArrayType(ParseType(), (int)n);
        }
        if (At(TokenKind.Identifier))
        {
            var t = Advance().Text;
            return t switch
            {
                "i8" => new PrimType(PrimKind.I8),
                "i16" => new PrimType(PrimKind.I16),
                "i32" => new PrimType(PrimKind.I32),
                "u8" => new PrimType(PrimKind.U8),
                "u16" => new PrimType(PrimKind.U16),
                "u32" => new PrimType(PrimKind.U32),
                "bool" => new PrimType(PrimKind.Bool),
                "void" => new PrimType(PrimKind.Void),
                _ => new NamedType(t)
            };
        }
        _diag.Error($"expected a type, found '{Cur.Text}'", Cur.Span);
        return new PrimType(PrimKind.Void);
    }

    // ---- statements ----
    private Block ParseBlock()
    {
        var start = Cur.Span;
        Expect(TokenKind.LBrace, "'{'");
        var stmts = new List<Stmt>();
        while (!At(TokenKind.RBrace) && !At(TokenKind.Eof))
        {
            int before = _i;
            var s = ParseStmt();
            if (s != null) stmts.Add(s);
            if (_i == before) SyncStmt();
        }
        Expect(TokenKind.RBrace, "'}'");
        return new Block(stmts) { Span = To(start) };
    }

    private void SyncStmt()
    {
        while (!At(TokenKind.Eof) && !At(TokenKind.Semicolon) && !At(TokenKind.RBrace)) Advance();
        Match(TokenKind.Semicolon);
    }

    private Stmt? ParseStmt()
    {
        var start = Cur.Span;
        switch (Cur.Kind)
        {
            case TokenKind.LBrace: return ParseBlock();
            case TokenKind.Let: return ParseLet(start);
            case TokenKind.If: return ParseIf(start);
            case TokenKind.While: return ParseWhile(start);
            case TokenKind.Return: return ParseReturn(start);
            default:
                {
                    // assignment or expression statement
                    var e = ParseExpr();
                    if (Match(TokenKind.Assign))
                    {
                        var rhs = ParseExpr();
                        Expect(TokenKind.Semicolon, "';'");
                        return new AssignStmt(e, rhs) { Span = To(start) };
                    }
                    Expect(TokenKind.Semicolon, "';'");
                    return new ExprStmt(e) { Span = To(start) };
                }
        }
    }

    private Stmt ParseLet(SourceSpan start)
    {
        Advance();
        var name = Expect(TokenKind.Identifier, "variable name").Text;
        TypeRef? ty = null;
        if (Match(TokenKind.Colon)) ty = ParseType();
        Expect(TokenKind.Assign, "'='");
        var init = ParseExpr();
        Expect(TokenKind.Semicolon, "';'");
        return new LetStmt(name, ty, init) { Span = To(start) };
    }

    private Stmt ParseIf(SourceSpan start)
    {
        Advance(); Expect(TokenKind.LParen, "'('");
        var cond = ParseExpr();
        Expect(TokenKind.RParen, "')'");
        var then = ParseBlock();
        Block? els = null;
        if (Match(TokenKind.Else)) els = At(TokenKind.If) ? WrapStmt(ParseIf(Cur.Span)) : ParseBlock();
        return new IfStmt(cond, then, els) { Span = To(start) };
    }

    private static Block WrapStmt(Stmt s) => new(new List<Stmt> { s }) { Span = s.Span };

    private Stmt ParseWhile(SourceSpan start)
    {
        Advance(); Expect(TokenKind.LParen, "'('");
        var cond = ParseExpr();
        Expect(TokenKind.RParen, "')'");
        var body = ParseBlock();
        return new WhileStmt(cond, body) { Span = To(start) };
    }

    private Stmt ParseReturn(SourceSpan start)
    {
        Advance();
        Expr? v = null;
        if (!At(TokenKind.Semicolon)) v = ParseExpr();
        Expect(TokenKind.Semicolon, "';'");
        return new ReturnStmt(v) { Span = To(start) };
    }

    // ---- expressions (precedence climbing) ----
    // levels: || , && , (== != < <= > >=) , (+ -) , (* / %) , unary , postfix , primary
    private Expr ParseExpr() => ParseOr();

    private Expr ParseOr()
    {
        var e = ParseAnd();
        while (At(TokenKind.OrOr)) { var s = e.Span; Advance(); e = new BinaryExpr(BinOp.Or, e, ParseAnd()) { Span = To(s) }; }
        return e;
    }
    private Expr ParseAnd()
    {
        var e = ParseCmp();
        while (At(TokenKind.AndAnd)) { var s = e.Span; Advance(); e = new BinaryExpr(BinOp.And, e, ParseCmp()) { Span = To(s) }; }
        return e;
    }
    private Expr ParseCmp()
    {
        var e = ParseAdd();
        while (true)
        {
            BinOp op;
            switch (Cur.Kind)
            {
                case TokenKind.Eq: op = BinOp.Eq; break;
                case TokenKind.Ne: op = BinOp.Ne; break;
                case TokenKind.Lt: op = BinOp.Lt; break;
                case TokenKind.Le: op = BinOp.Le; break;
                case TokenKind.Gt: op = BinOp.Gt; break;
                case TokenKind.Ge: op = BinOp.Ge; break;
                default: return e;
            }
            var s = e.Span; Advance(); e = new BinaryExpr(op, e, ParseAdd()) { Span = To(s) };
        }
    }
    private Expr ParseAdd()
    {
        var e = ParseMul();
        while (At(TokenKind.Plus) || At(TokenKind.Minus))
        {
            var op = At(TokenKind.Plus) ? BinOp.Add : BinOp.Sub;
            var s = e.Span; Advance(); e = new BinaryExpr(op, e, ParseMul()) { Span = To(s) };
        }
        return e;
    }
    private Expr ParseMul()
    {
        var e = ParseUnary();
        while (At(TokenKind.Star) || At(TokenKind.Slash) || At(TokenKind.Percent))
        {
            var op = Cur.Kind == TokenKind.Star ? BinOp.Mul : Cur.Kind == TokenKind.Slash ? BinOp.Div : BinOp.Mod;
            var s = e.Span; Advance(); e = new BinaryExpr(op, e, ParseUnary()) { Span = To(s) };
        }
        return e;
    }
    private Expr ParseUnary()
    {
        var start = Cur.Span;
        switch (Cur.Kind)
        {
            case TokenKind.Minus: Advance(); return new UnaryExpr(UnOp.Neg, ParseUnary()) { Span = To(start) };
            case TokenKind.Not: Advance(); return new UnaryExpr(UnOp.Not, ParseUnary()) { Span = To(start) };
            case TokenKind.Star: Advance(); return new UnaryExpr(UnOp.Deref, ParseUnary()) { Span = To(start) };
            case TokenKind.Amp: Advance(); return new UnaryExpr(UnOp.AddrOf, ParseUnary()) { Span = To(start) };
            default: return ParsePostfix();
        }
    }
    private Expr ParsePostfix()
    {
        var e = ParsePrimary();
        while (true)
        {
            var s = e.Span;
            if (Match(TokenKind.LParen))
            {
                var args = new List<Expr>();
                if (!At(TokenKind.RParen)) do { args.Add(ParseExpr()); } while (Match(TokenKind.Comma));
                Expect(TokenKind.RParen, "')'");
                e = new CallExpr(e, args) { Span = To(s) };
            }
            else if (Match(TokenKind.Dot))
            {
                var field = Expect(TokenKind.Identifier, "field name").Text;
                e = new MemberExpr(e, field) { Span = To(s) };
            }
            else if (Match(TokenKind.LBracket))
            {
                var idx = ParseExpr();
                Expect(TokenKind.RBracket, "']'");
                e = new IndexExpr(e, idx) { Span = To(s) };
            }
            else if (Match(TokenKind.As))
            {
                e = new CastExpr(e, ParseType()) { Span = To(s) };
            }
            else break;
        }
        return e;
    }
    private Expr ParsePrimary()
    {
        var start = Cur.Span;
        switch (Cur.Kind)
        {
            case TokenKind.IntLiteral: { var v = Advance().Value; return new IntLit(v) { Span = To(start) }; }
            case TokenKind.True: Advance(); return new BoolLit(true) { Span = To(start) };
            case TokenKind.False: Advance(); return new BoolLit(false) { Span = To(start) };
            case TokenKind.Identifier: { var n = Advance().Text; return new NameExpr(n) { Span = To(start) }; }
            case TokenKind.LParen: { Advance(); var e = ParseExpr(); Expect(TokenKind.RParen, "')'"); return e; }
            default:
                _diag.Error($"expected an expression, found '{Cur.Text}'", Cur.Span);
                if (!At(TokenKind.Eof)) Advance(); // ensure progress
                return new IntLit(0) { Span = To(start) };
        }
    }
}