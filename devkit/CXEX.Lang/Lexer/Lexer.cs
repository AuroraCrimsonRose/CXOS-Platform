using System.Collections.Generic;
using System.Globalization;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Lexer;

/// <summary>
/// X core v0.1 lexer. Single forward pass over UTF-8 source text, tracking line/col
/// for spans. Skips whitespace and // line + /* block */ comments. Reports lexical
/// errors into the bag but keeps going (emits an Error token and advances) so the
/// parser can recover and surface multiple diagnostics.
/// </summary>
public sealed class Lexer
{
    private readonly string _src;
    private readonly string _file;
    private readonly DiagnosticBag _diag;
    private int _pos, _line = 1, _col = 1;

    private static readonly Dictionary<string, TokenKind> Keywords = new()
    {
        ["fn"] = TokenKind.Fn,
        ["struct"] = TokenKind.Struct,
        ["global"] = TokenKind.Global,
        ["const"] = TokenKind.Const,
        ["extern"] = TokenKind.Extern,
        ["let"] = TokenKind.Let,
        ["if"] = TokenKind.If,
        ["else"] = TokenKind.Else,
        ["while"] = TokenKind.While,
        ["return"] = TokenKind.Return,
        ["as"] = TokenKind.As,
        ["true"] = TokenKind.True,
        ["false"] = TokenKind.False,
    };

    public Lexer(string source, string file, DiagnosticBag diag)
    { _src = source; _file = file; _diag = diag; }

    public List<Token> Tokenize()
    {
        var tokens = new List<Token>();
        Token t;
        do { t = Next(); tokens.Add(t); } while (t.Kind != TokenKind.Eof);
        return tokens;
    }

    private bool Eof => _pos >= _src.Length;
    private char Cur => Eof ? '\0' : _src[_pos];
    private char Peek(int n = 1) => _pos + n < _src.Length ? _src[_pos + n] : '\0';

    private void Advance()
    {
        if (Cur == '\n') { _line++; _col = 1; } else { _col++; }
        _pos++;
    }

    private void SkipTrivia()
    {
        while (!Eof)
        {
            char c = Cur;
            if (c is ' ' or '\t' or '\r' or '\n') { Advance(); }
            else if (c == '/' && Peek() == '/') { while (!Eof && Cur != '\n') Advance(); }
            else if (c == '/' && Peek() == '*')
            {
                Advance(); Advance();
                while (!Eof && !(Cur == '*' && Peek() == '/')) Advance();
                if (!Eof) { Advance(); Advance(); }
            }
            else break;
        }
    }

    private SourceSpan SpanFrom(int start, int line, int col) => new(_file, start, _pos, line, col);

    private Token Make(TokenKind k, int start, int line, int col, ulong val = 0)
        => new(k, _src.Substring(start, _pos - start), SpanFrom(start, line, col), val);

    private Token Next()
    {
        SkipTrivia();
        int start = _pos, line = _line, col = _col;
        if (Eof) return new(TokenKind.Eof, "", SpanFrom(start, line, col));

        char c = Cur;

        // identifier / keyword
        if (char.IsLetter(c) || c == '_')
        {
            while (!Eof && (char.IsLetterOrDigit(Cur) || Cur == '_')) Advance();
            string text = _src.Substring(start, _pos - start);
            return Keywords.TryGetValue(text, out var kw)
                ? Make(kw, start, line, col)
                : Make(TokenKind.Identifier, start, line, col);
        }

        // integer literal (decimal or 0x hex)
        if (char.IsDigit(c))
        {
            bool hex = c == '0' && (Peek() is 'x' or 'X');
            if (hex) { Advance(); Advance(); while (!Eof && Uri.IsHexDigit(Cur)) Advance(); }
            else { while (!Eof && char.IsDigit(Cur)) Advance(); }
            string text = _src.Substring(start, _pos - start);
            string digits = hex ? text.Substring(2) : text;
            if (digits.Length == 0 ||
                !ulong.TryParse(digits, hex ? NumberStyles.HexNumber : NumberStyles.None,
                                CultureInfo.InvariantCulture, out ulong v))
            {
                _diag.Error($"invalid integer literal '{text}'", SpanFrom(start, line, col));
                return Make(TokenKind.Error, start, line, col);
            }
            return Make(TokenKind.IntLiteral, start, line, col, v);
        }

        // operators & punctuation (longest match first)
        TokenKind k;
        switch (c)
        {
            case '(': Advance(); k = TokenKind.LParen; break;
            case ')': Advance(); k = TokenKind.RParen; break;
            case '{': Advance(); k = TokenKind.LBrace; break;
            case '}': Advance(); k = TokenKind.RBrace; break;
            case '[': Advance(); k = TokenKind.LBracket; break;
            case ']': Advance(); k = TokenKind.RBracket; break;
            case ',': Advance(); k = TokenKind.Comma; break;
            case ';': Advance(); k = TokenKind.Semicolon; break;
            case ':': Advance(); k = TokenKind.Colon; break;
            case '.': Advance(); k = TokenKind.Dot; break;
            case '+': Advance(); k = TokenKind.Plus; break;
            case '*': Advance(); k = TokenKind.Star; break;
            case '/': Advance(); k = TokenKind.Slash; break;
            case '%': Advance(); k = TokenKind.Percent; break;
            case '-': Advance(); if (Cur == '>') { Advance(); k = TokenKind.Arrow; } else k = TokenKind.Minus; break;
            case '=': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Eq; } else k = TokenKind.Assign; break;
            case '!': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Ne; } else k = TokenKind.Not; break;
            case '<': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Le; } else k = TokenKind.Lt; break;
            case '>': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Ge; } else k = TokenKind.Gt; break;
            case '&': Advance(); if (Cur == '&') { Advance(); k = TokenKind.AndAnd; } else k = TokenKind.Amp; break;
            case '|':
                Advance(); if (Cur == '|') { Advance(); k = TokenKind.OrOr; }
                else { _diag.Error("unexpected '|' (did you mean '||'?)", SpanFrom(start, line, col)); k = TokenKind.Error; }
                break;
            default:
                Advance();
                _diag.Error($"unexpected character '{c}'", SpanFrom(start, line, col));
                k = TokenKind.Error; break;
        }
        return Make(k, start, line, col);
    }
}