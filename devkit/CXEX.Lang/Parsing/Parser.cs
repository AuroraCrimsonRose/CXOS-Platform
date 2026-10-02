// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System.Collections.Generic;
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
    private static PrimKind? SuffixKind(string? s) => s switch
    {
        "u8" => PrimKind.U8, "u16" => PrimKind.U16, "u32" => PrimKind.U32, "u64" => PrimKind.U64, "u128" => PrimKind.U128,
        "i8" => PrimKind.I8, "i16" => PrimKind.I16, "i32" => PrimKind.I32, "i64" => PrimKind.I64, "i128" => PrimKind.I128,
        "char" => PrimKind.U8,
        _ => null,
    };

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
          or TokenKind.Const or TokenKind.Extern or TokenKind.Type or TokenKind.Enum;

    // ---- declarations ----
    private Decl? ParseDecl()
    {
        var attrs = ParseAttrs();
        var d = ParseDeclBody();
        if (d == null || attrs.Count == 0) return d;
        return d with { Attrs = attrs };
    }

    // @name  |  @name(arg, name = arg, ...)   - zero or more, before a declaration
    private List<Attr> ParseAttrs()
    {
        var list = new List<Attr>();
        while (At(TokenKind.At))
        {
            var start = Cur.Span;
            Advance();
            var name = Expect(TokenKind.Identifier, "an attribute name").Text;
            var args = new List<AttrArg>();
            if (Match(TokenKind.LParen))
            {
                if (!At(TokenKind.RParen))
                    do
                    {
                        var astart = Cur.Span;
                        string? argName = null;
                        if (At(TokenKind.Identifier) && Peek().Kind == TokenKind.Assign)
                        {
                            argName = Advance().Text;
                            Advance();   // '='
                        }
                        args.Add(new AttrArg(argName, ParseExpr()) { Span = To(astart) });
                    } while (Match(TokenKind.Comma));
                Expect(TokenKind.RParen, "')'");
            }
            list.Add(new Attr(name, args) { Span = To(start) });
        }
        return list;
    }

    private Decl? ParseDeclBody()
    {
        var start = Cur.Span;
        switch (Cur.Kind)
        {
            case TokenKind.Fn: return ParseFn(start, isExtern: false);
            case TokenKind.Extern: Advance(); return ParseFn(start, isExtern: true);  // ParseFn consumes 'fn'
            case TokenKind.Import:
                {
                    Advance();
                    var path = Expect(TokenKind.StringLiteral, "an import path string").Text;
                    Expect(TokenKind.Semicolon, "';'");
                    return new ImportDecl(path) { Span = To(start) };
                }
            case TokenKind.Type:
                {
                    Advance();
                    var an = Expect(TokenKind.Identifier, "alias name").Text;
                    Expect(TokenKind.Assign, "'='");
                    var at = ParseType();
                    Expect(TokenKind.Semicolon, "';'");
                    return new TypeAliasDecl(an, at) { Span = To(start) };
                }
            case TokenKind.Struct: return ParseStruct(start);
            case TokenKind.Enum: return ParseEnum(start);
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

    // enum name [: type] { a, b = 5, c { field: T, ... }, }
    private EnumDecl ParseEnum(SourceSpan start)
    {
        Advance(); // 'enum'
        var name = Expect(TokenKind.Identifier, "enum name").Text;
        TypeRef? backing = null;
        if (Match(TokenKind.Colon)) backing = ParseType();
        Expect(TokenKind.LBrace, "'{'");
        var vs = new List<EnumVariant>();
        while (!At(TokenKind.RBrace) && !At(TokenKind.Eof))
        {
            var vstart = Cur.Span;
            var vn = Expect(TokenKind.Identifier, "a variant name").Text;
            List<Param>? fields = null;
            Expr? value = null;
            if (Match(TokenKind.LBrace))
            {
                fields = new List<Param>();
                while (!At(TokenKind.RBrace) && !At(TokenKind.Eof))
                {
                    fields.Add(ParseParam());
                    if (!Match(TokenKind.Comma)) break;
                }
                Expect(TokenKind.RBrace, "'}'");
            }
            else if (Match(TokenKind.Assign)) value = ParseExpr();
            vs.Add(new EnumVariant(vn, fields, value) { Span = To(vstart) });
            if (!Match(TokenKind.Comma)) break;
        }
        Expect(TokenKind.RBrace, "'}'");
        return new EnumDecl(name, backing, vs) { Span = To(start) };
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
        if (Match(TokenKind.Star))
        {
            /* `user`, `phys` and `dma` are CONTEXTUAL: a qualifier only when
               they sit straight after `*` and another type follows. They are
               not reserved words, so a variable or a struct called `user` -
               which ordinary code has every reason to contain - still parses.
               `*user` on its own is a pointer to a struct named user; `*user
               u8` is a user-space pointer to u8. */
            var space = AddrSpace.Normal;
            if (At(TokenKind.Identifier) && Peek().Kind is TokenKind.Identifier or TokenKind.Star
                                                         or TokenKind.LBracket or TokenKind.Fn)
            {
                space = Cur.Text switch
                {
                    "user" => AddrSpace.User,
                    "phys" => AddrSpace.Phys,
                    "dma"  => AddrSpace.Dma,
                    _      => AddrSpace.Normal,
                };
                if (space != AddrSpace.Normal) Advance();
            }
            return new PointerType(ParseType(), space);
        }
        if (Match(TokenKind.LBracket))
        {
            UInt128 n = UInt128.Zero;
            if (At(TokenKind.IntLiteral)) n = Advance().Value;
            else _diag.Error("expected array length", Cur.Span);
            /* A literal is lexed at 128 bits now, so an array length has to be
               CHECKED rather than cast. Without this, [4294967296]u8 would
               truncate to 0 and silently produce a zero-length array instead
               of saying the length is impossible. */
            if (n > (UInt128)int.MaxValue)
            {
                _diag.Error("array length is too large", Cur.Span);
                n = UInt128.Zero;
            }
            Expect(TokenKind.RBracket, "']'");
            return new ArrayType(ParseType(), (int)n);
        }
        // function pointer type:  fn(T, U) -> R
        if (Match(TokenKind.Fn))
        {
            Expect(TokenKind.LParen, "'('");
            var ps = new List<TypeRef>();
            if (!At(TokenKind.RParen))
                do { ps.Add(ParseType()); } while (Match(TokenKind.Comma));
            Expect(TokenKind.RParen, "')'");
            TypeRef ret = new PrimType(PrimKind.Void);
            if (Match(TokenKind.Arrow)) ret = ParseType();
            return new FuncType(ps, ret);
        }
        if (At(TokenKind.Identifier))
        {
            var t = Advance().Text;
            return t switch
            {
                "i8" => new PrimType(PrimKind.I8),
                "i16" => new PrimType(PrimKind.I16),
                "i32" => new PrimType(PrimKind.I32),
                "i64" => new PrimType(PrimKind.I64),
                "i128" => new PrimType(PrimKind.I128),
                "u8" => new PrimType(PrimKind.U8),
                "u16" => new PrimType(PrimKind.U16),
                "u32" => new PrimType(PrimKind.U32),
                "u64" => new PrimType(PrimKind.U64),
                "u128" => new PrimType(PrimKind.U128),
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
            case TokenKind.Switch: return ParseSwitch(start);
            case TokenKind.Return: return ParseReturn(start);
            case TokenKind.Break:
                Advance(); Expect(TokenKind.Semicolon, "';'");
                return new BreakStmt() { Span = To(start) };
            case TokenKind.Continue:
                Advance(); Expect(TokenKind.Semicolon, "';'");
                return new ContinueStmt() { Span = To(start) };
            case TokenKind.Defer:
                {
                    // `defer stmt;` or `defer { ... }` - the body is any single
                    // statement, a block included
                    Advance();
                    var body = ParseStmt() ?? new Block(new List<Stmt>());
                    return new DeferStmt(body) { Span = To(start) };
                }
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
                    if (CompoundOp(Cur.Kind) is BinOp cop)
                    {
                        Advance();
                        var rhs = ParseExpr();
                        Expect(TokenKind.Semicolon, "';'");
                        return new AssignStmt(e, new BinaryExpr(cop, e, rhs) { Span = To(start) })
                               { Compound = true, Span = To(start) };
                    }
                    Expect(TokenKind.Semicolon, "';'");
                    return new ExprStmt(e) { Span = To(start) };
                }
        }
    }

    private static BinOp? CompoundOp(TokenKind k) => k switch
    {
        TokenKind.PlusAssign => BinOp.Add, TokenKind.MinusAssign => BinOp.Sub,
        TokenKind.StarAssign => BinOp.Mul, TokenKind.SlashAssign => BinOp.Div,
        TokenKind.PercentAssign => BinOp.Mod, TokenKind.AmpAssign => BinOp.BitAnd,
        TokenKind.PipeAssign => BinOp.BitOr, TokenKind.CaretAssign => BinOp.BitXor,
        TokenKind.ShlAssign => BinOp.Shl, TokenKind.ShrAssign => BinOp.Shr,
        _ => null,
    };

    private Stmt ParseLet(SourceSpan start)
    {
        Advance();
        var name = Expect(TokenKind.Identifier, "variable name").Text;
        TypeRef? ty = null;
        if (Match(TokenKind.Colon)) ty = ParseType();

        // `let x: T = expr;`  or  `let x: T;` (declared, uninitialized).
        // Without an initializer the type is REQUIRED - nothing to infer from.
        // Uninitialized locals are NOT zeroed (C semantics). This is what makes
        // local buffers/structs possible: `let buf: [64]u8;`
        Expr? init = null;
        if (Match(TokenKind.Assign)) init = ParseExpr();
        else if (ty == null)
            _diag.Error($"'{name}' needs either a type or an initializer", Cur.Span);

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

    /* switch (subject) { case L1, L2 { } case Enum.v(name) { } else { } }
       A label is a constant, or a variant; `Enum.v(name)` binds `name` to the
       variant's fields. Labels are parsed with record literals switched off,
       so `case color.red { }` is a label and a body, not a literal. */
    private Stmt ParseSwitch(SourceSpan start)
    {
        Advance(); Expect(TokenKind.LParen, "'('");
        var subject = ParseExpr();
        Expect(TokenKind.RParen, "')'");
        Expect(TokenKind.LBrace, "'{'");
        var cases = new List<SwitchCase>();
        Block? els = null;
        while (!At(TokenKind.RBrace) && !At(TokenKind.Eof))
        {
            var cs = Cur.Span;
            if (Match(TokenKind.Else))
            {
                if (els != null) _diag.Error("a switch has one 'else'", cs);
                els = ParseBlock();
                continue;
            }
            if (!Match(TokenKind.Case)) { _diag.Error($"expected 'case' or 'else', found '{Cur.Text}'", Cur.Span); Advance(); continue; }
            var labels = new List<Expr>();
            string? bind = null;
            _noRecordLit = true;
            do
            {
                var l = ParseExpr();
                if (l is CallExpr { Callee: MemberExpr mv, Args: [NameExpr bn] }) { bind = bn.Name; l = mv; }
                labels.Add(l);
            } while (Match(TokenKind.Comma));
            _noRecordLit = false;
            var body = ParseBlock();
            var c = new SwitchCase(labels, bind, body) { Span = To(cs) };
            if (bind != null) c = c with { BindLet = new LetStmt(bind, null, null) { Span = c.Span } };
            cases.Add(c);
        }
        Expect(TokenKind.RBrace, "'}'");
        return new SwitchStmt(subject, cases, els) { Span = To(start) };
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
        var e = ParseBitOr();
        while (At(TokenKind.AndAnd)) { var s = e.Span; Advance(); e = new BinaryExpr(BinOp.And, e, ParseBitOr()) { Span = To(s) }; }
        return e;
    }
    // C-style precedence:  |  ^  &  ==/!=  </<=/>/>=  <<//>>  +/-  */ /%
    private Expr ParseBitOr()
    {
        var e = ParseBitXor();
        while (At(TokenKind.Pipe)) { var s = e.Span; Advance(); e = new BinaryExpr(BinOp.BitOr, e, ParseBitXor()) { Span = To(s) }; }
        return e;
    }

    private Expr ParseBitXor()
    {
        var e = ParseBitAnd();
        while (At(TokenKind.Caret)) { var s = e.Span; Advance(); e = new BinaryExpr(BinOp.BitXor, e, ParseBitAnd()) { Span = To(s) }; }
        return e;
    }

    private Expr ParseBitAnd()
    {
        var e = ParseCmp();
        while (At(TokenKind.Amp)) { var s = e.Span; Advance(); e = new BinaryExpr(BinOp.BitAnd, e, ParseCmp()) { Span = To(s) }; }
        return e;
    }

    private Expr ParseCmp()
    {
        var e = ParseShift();
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
            var s = e.Span; Advance(); e = new BinaryExpr(op, e, ParseShift()) { Span = To(s) };
        }
    }
    private Expr ParseShift()
    {
        var e = ParseAdd();
        while (At(TokenKind.Shl) || At(TokenKind.Shr))
        {
            var s = e.Span;
            var op = Cur.Kind == TokenKind.Shl ? BinOp.Shl : BinOp.Shr;
            Advance();
            e = new BinaryExpr(op, e, ParseAdd()) { Span = To(s) };
        }
        return e;
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
            case TokenKind.Sizeof: Advance(); return new SizeofExpr(ParseUnary()) { Span = To(start) };
            case TokenKind.Minus: Advance(); return new UnaryExpr(UnOp.Neg, ParseUnary()) { Span = To(start) };
            case TokenKind.Not: Advance(); return new UnaryExpr(UnOp.Not, ParseUnary()) { Span = To(start) };
            case TokenKind.Tilde: Advance(); return new UnaryExpr(UnOp.BitNot, ParseUnary()) { Span = To(start) };
            case TokenKind.Star: Advance(); return new UnaryExpr(UnOp.Deref, ParseUnary()) { Span = To(start) };
            case TokenKind.Amp: Advance(); return new UnaryExpr(UnOp.AddrOf, ParseUnary()) { Span = To(start) };
            default: return ParsePostfix();
        }
    }
    /* Inside a `case` label a name followed by `{` is the label and its body,
       not a record literal; this switches the literal off while one is parsed. */
    private bool _noRecordLit;

    /* `Name {` starts a record literal only when what follows is `}` or
       `field:` - a block never starts that way, so `while (x) { ... }` and the
       like are untouched. */
    private bool AtRecordLit() =>
        !_noRecordLit && At(TokenKind.LBrace) &&
        (Peek().Kind == TokenKind.RBrace ||
         (Peek().Kind == TokenKind.Identifier && Peek(2).Kind == TokenKind.Colon));

    private Expr ParseRecordLit(Expr typeName)
    {
        var s = typeName.Span;
        Expect(TokenKind.LBrace, "'{'");
        var fields = new List<FieldInit>();
        while (!At(TokenKind.RBrace) && !At(TokenKind.Eof))
        {
            var fs = Cur.Span;
            var name = Expect(TokenKind.Identifier, "a field name").Text;
            Expect(TokenKind.Colon, "':'");
            fields.Add(new FieldInit(name, ParseExpr()) { Span = To(fs) });
            if (!Match(TokenKind.Comma)) break;
        }
        Expect(TokenKind.RBrace, "'}'");
        return new StructLit(typeName, fields) { Span = To(s) };
    }

    private Expr ParsePostfix()
    {
        var e = ParsePrimary();
        if (e is NameExpr && AtRecordLit()) e = ParseRecordLit(e);
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
                if (e is MemberExpr { Target: NameExpr } && AtRecordLit()) e = ParseRecordLit(e);   // Enum.variant { ... }
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
            case TokenKind.IntLiteral:
                {
                    var tok = Advance();
                    return new IntLit(tok.Value) { Suffix = SuffixKind(tok.Suffix), IsChar = tok.Suffix == "char", Span = To(start) };
                }
            case TokenKind.StringLiteral: { var sv = Advance().Text; return new StrLit(sv) { Span = To(start) }; }
            case TokenKind.True: Advance(); return new BoolLit(true) { Span = To(start) };
            case TokenKind.False: Advance(); return new BoolLit(false) { Span = To(start) };
            case TokenKind.Identifier: { var n = Advance().Text; return new NameExpr(n) { Span = To(start) }; }
            case TokenKind.LParen: { Advance(); var e = ParseExpr(); Expect(TokenKind.RParen, "')'"); return e; }
            case TokenKind.LBracket:
                {
                    Advance();
                    var items = new List<Expr>();
                    while (!At(TokenKind.RBracket) && !At(TokenKind.Eof))
                    {
                        items.Add(ParseExpr());
                        if (!Match(TokenKind.Comma)) break;
                    }
                    Expect(TokenKind.RBracket, "']'");
                    return new ArrayLit(items) { Span = To(start) };
                }
            default:
                _diag.Error($"expected an expression, found '{Cur.Text}'", Cur.Span);
                if (!At(TokenKind.Eof)) Advance(); // ensure progress
                return new IntLit(0) { Span = To(start) };
        }
    }
}