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
        ["enum"] = TokenKind.Enum,
        ["switch"] = TokenKind.Switch,
        ["case"] = TokenKind.Case,
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
        ["import"] = TokenKind.Import,
        ["type"] = TokenKind.Type,
        ["sizeof"] = TokenKind.Sizeof,
        ["break"] = TokenKind.Break,
        ["continue"] = TokenKind.Continue,
        ["defer"] = TokenKind.Defer,
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

    private Token Make(TokenKind k, int start, int line, int col, UInt128 val = default, string? suffix = null)
        => new(k, _src.Substring(start, _pos - start), SpanFrom(start, line, col), val, suffix);

    /* `op=` if an `=` follows the operator just consumed, else the operator. */
    private TokenKind Eq2(TokenKind withEq, TokenKind plain)
    {
        if (!Eof && Cur == '=') { Advance(); return withEq; }
        return plain;
    }

    /* The integer types a literal can be written as. */
    private static readonly string[] IntSuffixes =
        { "u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64", "i128" };

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

        // string literal "..." with escapes; Text holds the DECODED bytes
        if (c == '"')
        {
            Advance(); // opening quote
            var sb = new System.Text.StringBuilder();
            while (!Eof && Cur != '"')
            {
                if (Cur == '\\')
                {
                    Advance();
                    char e = Cur;
                    Advance();
                    sb.Append(e switch
                    {
                        'n'  => '\n',
                        't'  => '\t',
                        'r'  => '\r',
                        '0'  => '\0',
                        'b'  => '\b',   // backspace (0x08) - the shell's "\b \b" erase
                        'f'  => '\f',   // form feed  (0x0C)
                        'v'  => '\v',   // vertical tab (0x0B)
                        'a'  => '\a',   // bell (0x07)
                        'e'  => '\u001b', // escape (0x1B) - handy for ANSI later
                        '\\' => '\\',
                        '"'  => '"',
                        '\'' => '\'',
                        _ => e   // unknown escape: keep the char literally
                    });
                }
                else { sb.Append(Cur); Advance(); }
            }
            if (Eof) { _diag.Error("unterminated string literal", SpanFrom(start, line, col)); return Make(TokenKind.Error, start, line, col); }
            Advance(); // closing quote
            return new Token(TokenKind.StringLiteral, sb.ToString(), SpanFrom(start, line, col));
        }

        /* A character literal is an integer literal: 'a' is the byte 97, typed
           u8. A lexer written in X is mostly comparisons against characters,
           and without this they were all magic numbers - `c == 34` where the
           meaning is '"'. Same escapes as a string, plus \xNN for any byte.
           One byte only: a character outside ASCII is several bytes in UTF-8,
           so it is a string's job, and refusing it beats picking one byte. */
        if (c == '\'')
        {
            Advance();
            int value = -1;
            if (!Eof && Cur == '\\')
            {
                Advance();
                char e = Eof ? '\0' : Cur;
                if (!Eof) Advance();
                if (e is 'x' or 'X')
                {
                    int hstart = _pos;
                    while (!Eof && _pos - hstart < 2 && Uri.IsHexDigit(Cur)) Advance();
                    if (_pos - hstart == 2) value = Convert.ToInt32(_src.Substring(hstart, 2), 16);
                }
                else value = e switch
                {
                    'n' => '\n', 't' => '\t', 'r' => '\r', '0' => 0, 'b' => '\b', 'f' => '\f', 'v' => '\v',
                    'a' => '\a', 'e' => 0x1B, '\\' => '\\', '\'' => '\'', '"' => '"',
                    _ => -1,
                };
            }
            else if (!Eof && Cur != '\'' && Cur != '\n')
            {
                if (Cur < 0x80) value = Cur;
                Advance();
            }
            if (Eof || Cur != '\'' || value < 0)
            {
                while (!Eof && Cur != '\'' && Cur != '\n') Advance();
                if (!Eof && Cur == '\'') Advance();
                _diag.Error("a character literal is one ASCII character or escape, in single quotes: 'a', '\\n', '\\x7F'",
                            SpanFrom(start, line, col));
                return Make(TokenKind.Error, start, line, col);
            }
            Advance();   // closing quote
            return Make(TokenKind.IntLiteral, start, line, col, (UInt128)value, "char");   // typed u8; see IntLit.IsChar
        }

        // integer literal (decimal or 0x hex)
        if (char.IsDigit(c))
        {
            /* `_` separates digit groups, as in X Data: 1_000_000, 0xFFFF_0000.
               A suffix names the literal's type outright - 1u128, 0xFFu8, 5i64 -
               which is how a constant gets a width its digits do not imply:
               `1u128 << 100` is a 128-bit shift, where `1 << 100` is a 32-bit
               one (and refused). */
            bool hex = c == '0' && (Peek() is 'x' or 'X');
            if (hex) { Advance(); Advance(); while (!Eof && (Uri.IsHexDigit(Cur) || Cur == '_')) Advance(); }
            else { while (!Eof && (char.IsDigit(Cur) || Cur == '_')) Advance(); }
            int digitsEnd = _pos;
            string? suffix = null;
            if (!Eof && (char.IsLetter(Cur) || Cur == '_'))
            {
                int s0 = _pos;
                while (!Eof && (char.IsLetterOrDigit(Cur) || Cur == '_')) Advance();
                suffix = _src.Substring(s0, _pos - s0);
                if (Array.IndexOf(IntSuffixes, suffix) < 0)
                {
                    _diag.Error($"'{suffix}' is not an integer type; a literal suffix is one of {string.Join(", ", IntSuffixes)}",
                                SpanFrom(start, line, col));
                    return Make(TokenKind.Error, start, line, col);
                }
            }
            string text = _src.Substring(start, _pos - start);
            string raw = _src.Substring(start, digitsEnd - start);
            string digits = (hex ? raw.Substring(2) : raw).Replace("_", "");
            /* Parsed at 128 bits, not 64, because u128 is a type in this
               language and a constant of it has to be writable. A literal
               wider than this is rejected rather than wrapped - there is no
               type that could hold it, so silently keeping the low bits would
               be a wrong answer with no way to notice. */
            if (digits.Length == 0 ||
                !UInt128.TryParse(digits, hex ? NumberStyles.HexNumber : NumberStyles.None,
                                  CultureInfo.InvariantCulture, out UInt128 v))
            {
                _diag.Error($"invalid integer literal '{text}'", SpanFrom(start, line, col));
                return Make(TokenKind.Error, start, line, col);
            }
            return Make(TokenKind.IntLiteral, start, line, col, v, suffix);
        }

        // operators & punctuation (longest match first)
        TokenKind k;
        switch (c)
        {
            case '@': Advance(); k = TokenKind.At; break;
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
            case '+': Advance(); k = Eq2(TokenKind.PlusAssign, TokenKind.Plus); break;
            case '*': Advance(); k = Eq2(TokenKind.StarAssign, TokenKind.Star); break;
            case '/': Advance(); k = Eq2(TokenKind.SlashAssign, TokenKind.Slash); break;
            case '%': Advance(); k = Eq2(TokenKind.PercentAssign, TokenKind.Percent); break;
            case '-': Advance(); if (Cur == '>') { Advance(); k = TokenKind.Arrow; } else k = Eq2(TokenKind.MinusAssign, TokenKind.Minus); break;
            case '=': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Eq; } else k = TokenKind.Assign; break;
            case '!': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Ne; } else k = TokenKind.Not; break;
            case '<': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Le; } else if (Cur == '<') { Advance(); k = Eq2(TokenKind.ShlAssign, TokenKind.Shl); } else k = TokenKind.Lt; break;
            case '|': Advance(); if (Cur == '|') { Advance(); k = TokenKind.OrOr; } else k = Eq2(TokenKind.PipeAssign, TokenKind.Pipe); break;
            case '^': Advance(); k = Eq2(TokenKind.CaretAssign, TokenKind.Caret); break;
            case '~': Advance(); k = TokenKind.Tilde; break;
            case '>': Advance(); if (Cur == '=') { Advance(); k = TokenKind.Ge; } else if (Cur == '>') { Advance(); k = Eq2(TokenKind.ShrAssign, TokenKind.Shr); } else k = TokenKind.Gt; break;
            case '&': Advance(); if (Cur == '&') { Advance(); k = TokenKind.AndAnd; } else k = Eq2(TokenKind.AmpAssign, TokenKind.Amp); break;
            default:
                Advance();
                _diag.Error($"unexpected character '{c}'", SpanFrom(start, line, col));
                k = TokenKind.Error; break;
        }
        return Make(k, start, line, col);
    }
}