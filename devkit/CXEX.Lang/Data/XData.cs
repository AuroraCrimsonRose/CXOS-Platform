using System;
using System.Collections.Generic;
using System.Text;

namespace CXEX.Lang.Data;

/// <summary>X Data error codes. The NUMBERS are the same as <c>XD_E_*</c> in CXK's
/// <c>os/std/xdata.xfxn</c>, so a result from either reader means the same thing.</summary>
public enum XDataError
{
    Ok     = 0,
    Syntax = -1,   // a character that cannot start or continue what is expected
    String = -2,   // unterminated string, raw newline inside one, unknown escape
    Depth  = -3,   // nested deeper than MaxDepth
    DupKey = -4,   // a key repeated in one record
    Sep    = -5,   // two entries or items on one line with no comma
    Range  = -6,   // an integer that does not fit the type asked for
    Type   = -7,   // not the kind of value asked for
    Space  = -8,   // the caller's buffer is too small (never raised here)
}

public enum XDataKind { None = 0, Str = 1, Int = 2, Bool = 3, Sym = 4, List = 5, Rec = 6 }

/// <summary>A value: its kind and where it sits in the source. For a list or record,
/// <see cref="At"/>/<see cref="Len"/> are the BODY - the text between the brackets - exactly
/// as in the X reader.</summary>
public readonly record struct XDataValue(XDataKind Kind, int At, int Len, int TagAt = 0, int TagLen = 0);

/// <summary>
/// The DevKit's X Data reader - a deliberate, line-for-line port of CXK's
/// <c>os/std/xdata.xfxn</c>, specified in <c>docs/language/CX_X_DATA.md</c>.
///
/// <para><b>Why a port and not a fresh implementation.</b> Two readers of one format are only
/// useful if they agree on what is valid, and agreement is far easier to keep - and to prove -
/// when both do the same walk in the same order. The build checks descriptors with this one and
/// the supervisor reads them with the other; a document that passed here and failed there would
/// be worse than no build-time check at all. So the structure, the error codes and the byte
/// offsets match the X reader exactly, and a differential test holds them to it.</para>
///
/// <para>Works on the raw UTF-8 bytes, not a decoded string, for the same reason: an offset has
/// to mean the same byte in both readers.</para>
/// </summary>
public sealed class XData
{
    public const int MaxDepth = 32;
    private const int Bad = -1;

    private readonly byte[] _s;
    private bool _nl;
    private int _kAt, _kLen, _vAt, _vEnd;

    public XDataError Error { get; private set; }
    public int ErrorAt { get; private set; }

    public XData(byte[] source) { _s = source; }
    public static XData FromText(string text) => new(Encoding.UTF8.GetBytes(text));

    public int Length => _s.Length;

    // ---- characters ----
    private static bool Digit(byte c) => c >= '0' && c <= '9';
    private static bool Hex(byte c) => Digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    private static bool IdentStart(byte c) => (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    private static bool Ident(byte c) => IdentStart(c) || Digit(c) || c == '-';

    private int Fail(XDataError code, int at) { Error = code; ErrorAt = at; return Bad; }

    private int IdentEnd(int p, int end) { while (p < end && Ident(_s[p])) p++; return p; }

    private bool Spells(int at, int len, string word)
    {
        if (len != word.Length) return false;
        for (int i = 0; i < len; i++) if (_s[at + i] != (byte)word[i]) return false;
        return true;
    }

    private bool Same(int a, int alen, int b, int blen)
    {
        if (alen != blen) return false;
        for (int i = 0; i < alen; i++) if (_s[a + i] != _s[b + i]) return false;
        return true;
    }

    // Skip whitespace and comments; set _nl if a line break was crossed.
    private int Skip(int p, int end)
    {
        _nl = false;
        while (p < end)
        {
            byte c = _s[p];
            if (c == '\n') { _nl = true; p++; continue; }
            if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
            if (c == '/' && p + 1 < end)
            {
                if (_s[p + 1] == '/') { while (p < end && _s[p] != '\n') p++; continue; }
                if (_s[p + 1] == '*')
                {
                    int q = p + 2;
                    while (q + 1 < end && !(_s[q] == '*' && _s[q + 1] == '/'))
                    {
                        if (_s[q] == '\n') _nl = true;
                        q++;
                    }
                    if (q + 1 >= end) return Fail(XDataError.Syntax, p);
                    p = q + 2;
                    continue;
                }
            }
            return p;
        }
        return p;
    }

    private int StringEnd(int p, int end)
    {
        int q = p + 1;
        while (q < end)
        {
            byte c = _s[q];
            if (c == '"') return q + 1;
            if (c == '\n') return Fail(XDataError.String, q);
            if (c == '\\')
            {
                if (q + 1 >= end) return Fail(XDataError.String, q);
                byte e = _s[q + 1];
                if (!(e == 'n' || e == 't' || e == 'r' || e == '0' || e == '\\' || e == '"'))
                    return Fail(XDataError.String, q);
                q += 2;
                continue;
            }
            q++;
        }
        return Fail(XDataError.String, p);
    }

    private int IntEnd(int p, int end)
    {
        int q = p;
        if (_s[q] == '-') q++;
        if (q >= end || !Digit(_s[q])) return Fail(XDataError.Syntax, p);
        if (_s[q] == '0' && q + 1 < end && (_s[q + 1] == 'x' || _s[q + 1] == 'X'))
        {
            q += 2;
            if (q >= end || !Hex(_s[q])) return Fail(XDataError.Syntax, p);
            while (q < end && (Hex(_s[q]) || _s[q] == '_')) q++;
        }
        else
        {
            while (q < end && (Digit(_s[q]) || _s[q] == '_')) q++;
        }
        if (q < end && Ident(_s[q])) return Fail(XDataError.Syntax, q);
        return q;
    }

    private int Entries(int p, int end, int depth)
    {
        if (depth > MaxDepth) return Fail(XDataError.Depth, p);
        int start = p;
        bool first = true, sep = false;
        while (true)
        {
            p = Skip(p, end);
            if (p == Bad) return Bad;
            if (_nl) sep = true;
            if (p >= end) return end;
            if (!first && !sep) return Fail(XDataError.Sep, p);

            if (!IdentStart(_s[p])) return Fail(XDataError.Syntax, p);
            int k = p;
            int ke = IdentEnd(p, end);
            if (FindKey(start, k, k, ke - k) != 0) return Fail(XDataError.DupKey, k);

            p = Skip(ke, end);
            if (p == Bad) return Bad;
            if (p >= end || _s[p] != '=') return Fail(XDataError.Syntax, p);
            p = Skip(p + 1, end);
            if (p == Bad) return Bad;
            p = Value(p, end, depth);
            if (p == Bad) return Bad;

            p = Skip(p, end);
            if (p == Bad) return Bad;
            sep = _nl;
            if (p < end && _s[p] == ',') { sep = true; p++; }
            first = false;
        }
    }

    private int Items(int p, int end, int depth)
    {
        if (depth > MaxDepth) return Fail(XDataError.Depth, p);
        bool first = true, sep = false;
        while (true)
        {
            p = Skip(p, end);
            if (p == Bad) return Bad;
            if (_nl) sep = true;
            if (p >= end) return Fail(XDataError.Syntax, p);
            if (_s[p] == ']') return p + 1;
            if (!first && !sep) return Fail(XDataError.Sep, p);
            p = Value(p, end, depth);
            if (p == Bad) return Bad;
            p = Skip(p, end);
            if (p == Bad) return Bad;
            sep = _nl;
            if (p < end && _s[p] == ',') { sep = true; p++; }
            first = false;
        }
    }

    private int CloseBrace(int p, int end)
    {
        int level = 1;
        while (p < end)
        {
            byte c = _s[p];
            if (c == '"')
            {
                p = StringEnd(p, end);
                if (p == Bad) return Bad;
                continue;
            }
            if (c == '/' && p + 1 < end && (_s[p + 1] == '/' || _s[p + 1] == '*'))
            {
                p = Skip(p, end);
                if (p == Bad) return Bad;
                continue;
            }
            if (c == '{') level++;
            if (c == '}') { level--; if (level == 0) return p; }
            p++;
        }
        return Fail(XDataError.Syntax, p);
    }

    private int Value(int p, int end, int depth)
    {
        if (p >= end) return Fail(XDataError.Syntax, p);
        byte c = _s[p];
        if (c == '"') return StringEnd(p, end);
        if (c == '[') return Items(p + 1, end, depth + 1);
        if (c == '{') return Record(p + 1, end, depth + 1);
        if (c == '-' || Digit(c)) return IntEnd(p, end);
        if (IdentStart(c))
        {
            int q = IdentEnd(p, end);
            int r = Skip(q, end);
            if (r == Bad) return Bad;
            if (r < end && _s[r] == '{' && !_nl) return Record(r + 1, end, depth + 1);
            return q;
        }
        return Fail(XDataError.Syntax, p);
    }

    private int Record(int p, int end, int depth)
    {
        int close = CloseBrace(p, end);
        if (close == Bad) return Bad;
        if (Entries(p, close, depth) == Bad) return Bad;
        return close + 1;
    }

    // Walk one entry of an already-checked body; sets _k*/_v*.
    private int Entry(int p, int end)
    {
        p = Skip(p, end);
        if (p == Bad || p >= end) return Bad;
        int k = p;
        int ke = IdentEnd(p, end);
        p = Skip(ke, end);
        p = Skip(p + 1, end);
        int v = p;
        int ve = Value(p, end, 0);
        _kAt = k; _kLen = ke - k; _vAt = v; _vEnd = ve;
        p = Skip(ve, end);
        if (p < end && _s[p] == ',') p++;
        return p;
    }

    private int FindKey(int from, int upto, int kat, int klen)
    {
        int p = from;
        while (p < upto)
        {
            int n = Entry(p, upto);
            if (n == Bad) return 0;
            if (Same(_kAt, _kLen, kat, klen)) return _vAt + 1;
            p = n;
        }
        return 0;
    }

    // ---- public: checking ----

    /// <summary>Validate the whole document. <see cref="XDataError.Ok"/>, or an error with
    /// <see cref="ErrorAt"/> set to the byte offset where it went wrong.</summary>
    public XDataError Check()
    {
        Error = XDataError.Ok;
        ErrorAt = 0;
        return Entries(0, _s.Length, 0) == Bad ? Error : XDataError.Ok;
    }

    /// <summary>The 1-based line and column of a byte offset.</summary>
    public (int Line, int Column) Position(int at)
    {
        int line = 1, col = 1;
        for (int i = 0; i < at && i < _s.Length; i++)
        {
            if (_s[i] == '\n') { line++; col = 1; } else col++;
        }
        return (line, col);
    }

    public static string Describe(XDataError code) => code switch
    {
        XDataError.Ok     => "ok",
        XDataError.Syntax => "unexpected character",
        XDataError.String => "unterminated string, line break inside a string, or unknown escape",
        XDataError.Depth  => "nested too deeply",
        XDataError.DupKey => "key given more than once",
        XDataError.Sep    => "two entries on one line with no comma",
        XDataError.Range  => "number out of range",
        XDataError.Type   => "wrong kind of value",
        XDataError.Space  => "value too long",
        _                 => "unknown error",
    };

    // ---- public: navigating (only after Check() succeeded) ----

    private XDataValue DescribeValue(int at, int end)
    {
        byte c = _s[at];
        if (c == '"') return new(XDataKind.Str, at, end - at);
        if (c == '-' || Digit(c)) return new(XDataKind.Int, at, end - at);
        if (c == '[') return new(XDataKind.List, at + 1, end - at - 2);
        if (c == '{') return new(XDataKind.Rec, at + 1, end - at - 2);
        int q = IdentEnd(at, end);
        if (q < end)
        {
            int r = Skip(q, end);
            return new(XDataKind.Rec, r + 1, end - r - 2, at, q - at);
        }
        if (Spells(at, end - at, "true") || Spells(at, end - at, "false")) return new(XDataKind.Bool, at, end - at);
        return new(XDataKind.Sym, at, end - at);
    }

    public XDataValue Root => new(XDataKind.Rec, 0, _s.Length);

    /// <summary>Every entry of a record, in order: its key's text, the key's offset, and the value.</summary>
    public IEnumerable<(string Key, int KeyAt, XDataValue Value)> Entries(XDataValue rec)
    {
        if (rec.Kind != XDataKind.Rec) yield break;
        int end = rec.At + rec.Len, p = rec.At;
        while (p < end)
        {
            int n = Entry(p, end);
            if (n == Bad) yield break;
            int kAt = _kAt, kLen = _kLen;
            var v = DescribeValue(_vAt, _vEnd);
            yield return (Encoding.ASCII.GetString(_s, kAt, kLen), kAt, v);
            p = n;
        }
    }

    public bool TryGet(XDataValue rec, string key, out XDataValue value)
    {
        foreach (var (k, _, v) in Entries(rec))
            if (k == key) { value = v; return true; }
        value = default;
        return false;
    }

    /// <summary>Every item of a list, in order.</summary>
    public IEnumerable<XDataValue> Items(XDataValue list)
    {
        if (list.Kind != XDataKind.List) yield break;
        int end = list.At + list.Len, p = list.At;
        while (true)
        {
            p = Skip(p, end);
            if (p >= end) yield break;
            int e = Value(p, end, 0);
            yield return DescribeValue(p, e);
            p = Skip(e, end);
            if (p < end && _s[p] == ',') p++;
        }
    }

    public string Str(XDataValue v)
    {
        if (v.Kind != XDataKind.Str) throw new InvalidOperationException("not a string");
        var sb = new List<byte>();
        for (int p = v.At + 1, end = v.At + v.Len - 1; p < end; p++)
        {
            byte c = _s[p];
            if (c == '\\')
            {
                byte e = _s[++p];
                c = e switch { (byte)'n' => (byte)'\n', (byte)'t' => (byte)'\t', (byte)'r' => (byte)'\r', (byte)'0' => 0, _ => e };
            }
            sb.Add(c);
        }
        return Encoding.UTF8.GetString(sb.ToArray());
    }

    public string Text(XDataValue v) => Encoding.UTF8.GetString(_s, v.At, v.Len);
}
