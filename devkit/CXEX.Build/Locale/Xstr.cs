// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace CXEX.Build.Locale;

/// <summary>
/// XSTR - a locale's string table (system/CX_TOOLKIT.md §10): fixed records
/// a reader checks with comparisons, as XFNT is, produced here from the
/// Fluent sources and read on CXOS by std/locale.xfxn (lc_parse). The
/// ceilings below are the X reader's own, so a table the DevKit writes is
/// one CXOS reads.
///
/// <code>
///  0 'XSTR'   4 version16   6 flags16 (bit 0 right-to-left)   8 tag[16]
/// 24 count32 28 index32 (= 40)  32 plurals32  36 size32 (= the length)
/// index: count records of key32 keylen16 bodylen16 body32, keys rising
/// body (a pattern): 1 len16 text | 2 len8 name | 3 len8 name count8 variants
/// variant: kind8 (0 category: cat8 | 1 integer: i32) default8 len16 pattern
/// plurals: count8, then per rule cat8 len16 program (Plurals.Compile)
/// </code>
/// </summary>
public static class Xstr
{
    public const uint Magic = 0x52545358;       // 'X','S','T','R' little-endian
    public const ushort Version = 1;
    public const int HeaderSize = 40;
    public const int RecordSize = 12;
    public const ushort FlagRtl = 1;

    public const int MaxSize = 16384;           // LC_TABLE_MAX
    public const int MaxKey = 64;
    public const int MaxName = 32;
    public const int MaxDepth = 4;
    public const int TagBytes = 16;

    public const byte OpText = 1, OpVar = 2, OpSelect = 3;

    public sealed record Table(string Tag, bool Rtl, SortedDictionary<string, byte[]> Bodies, byte[] PluralBlock);

    // ---- writing ----

    public static byte[] Pattern(List<Fluent.Element> p, int depth = 1)
    {
        if (depth > MaxDepth) throw new InvalidDataException($"selects nested deeper than {MaxDepth}");
        var o = new List<byte>();
        foreach (var e in p)
        {
            switch (e)
            {
                case Fluent.Text t:
                {
                    byte[] b = Encoding.UTF8.GetBytes(t.Value);
                    for (int at = 0; at < b.Length; )
                    {
                        // a text longer than a length holds is two texts, split on a character's start
                        int n = Math.Min(b.Length - at, 0xFFFF);
                        while (at + n < b.Length && (b[at + n] & 0xC0) == 0x80) n--;
                        o.Add(OpText); o.Add((byte)n); o.Add((byte)(n >> 8));
                        o.AddRange(b.AsSpan(at, n).ToArray());
                        at += n;
                    }
                    break;
                }
                case Fluent.Var v:
                    o.Add(OpVar); Name(o, v.Name);
                    break;
                case Fluent.Select s:
                    o.Add(OpSelect); Name(o, s.Name);
                    if (s.Variants.Count > 255) throw new InvalidDataException("a select of more than 255 variants");
                    o.Add((byte)s.Variants.Count);
                    foreach (var v in s.Variants)
                    {
                        if (v.IsNumber) { o.Add(1); o.AddRange(BitConverter.GetBytes(v.Number)); }
                        else { o.Add(0); o.Add((byte)v.Category); }
                        o.Add(v.IsDefault ? (byte)1 : (byte)0);
                        byte[] body = Pattern(v.Pattern, depth + 1);
                        if (body.Length > 0xFFFF) throw new InvalidDataException("a variant longer than 65535 bytes");
                        o.Add((byte)body.Length); o.Add((byte)(body.Length >> 8));
                        o.AddRange(body);
                    }
                    break;
            }
        }
        return o.ToArray();
    }

    private static void Name(List<byte> o, string name)
    {
        byte[] b = Encoding.UTF8.GetBytes(name);
        if (b.Length == 0 || b.Length > MaxName) throw new InvalidDataException($"variable name '{name}': 1 to {MaxName} bytes");
        o.Add((byte)b.Length); o.AddRange(b);
    }

    public static byte[] Write(Table t)
    {
        if (t.Tag.Length == 0 || t.Tag.Length >= TagBytes || !t.Tag.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'))
            throw new InvalidDataException($"locale tag '{t.Tag}': 1 to {TagBytes - 1} letters, digits and -");
        var keys = t.Bodies.Keys.ToList();
        keys.Sort(StringComparer.Ordinal);
        var data = new List<byte>();
        var recs = new List<(int Key, int KeyLen, int Body, int BodyLen)>();
        int baseOff = HeaderSize + RecordSize * keys.Count;
        foreach (string k in keys)
        {
            byte[] kb = Encoding.UTF8.GetBytes(k);
            if (kb.Length == 0 || kb.Length > MaxKey) throw new InvalidDataException($"key '{k}': 1 to {MaxKey} bytes");
            byte[] body = t.Bodies[k];
            if (body.Length > 0xFFFF) throw new InvalidDataException($"'{k}' is longer than 65535 bytes");
            int ko = baseOff + data.Count; data.AddRange(kb);
            int bo = baseOff + data.Count; data.AddRange(body);
            recs.Add((ko, kb.Length, bo, body.Length));
        }
        int plurals = baseOff + data.Count;
        data.AddRange(t.PluralBlock);
        int size = baseOff + data.Count;
        if (size > MaxSize) throw new InvalidDataException($"the table is {size} bytes; CXOS reads at most {MaxSize}");

        var o = new List<byte>(size);
        o.AddRange(BitConverter.GetBytes(Magic));
        o.AddRange(BitConverter.GetBytes(Version));
        o.AddRange(BitConverter.GetBytes(t.Rtl ? FlagRtl : (ushort)0));
        for (int k = 0; k < TagBytes; k++) o.Add(k < t.Tag.Length ? (byte)t.Tag[k] : (byte)0);
        o.AddRange(BitConverter.GetBytes((uint)keys.Count));
        o.AddRange(BitConverter.GetBytes((uint)HeaderSize));
        o.AddRange(BitConverter.GetBytes((uint)plurals));
        o.AddRange(BitConverter.GetBytes((uint)size));
        foreach (var r in recs)
        {
            o.AddRange(BitConverter.GetBytes((uint)r.Key));
            o.AddRange(BitConverter.GetBytes((ushort)r.KeyLen));
            o.AddRange(BitConverter.GetBytes((ushort)r.BodyLen));
            o.AddRange(BitConverter.GetBytes((uint)r.Body));
        }
        o.AddRange(data);
        return o.ToArray();
    }

    // ---- reading: the same checks lc_parse makes ----

    public sealed record ReadTable(string Tag, bool Rtl, Dictionary<string, byte[]> Bodies, byte[] PluralBlock);

    public static ReadTable Read(byte[] b)
    {
        static InvalidDataException Bad(string w) => new($"not a valid XSTR table: {w}");
        if (b.Length < HeaderSize) throw Bad("shorter than its header");
        if (b.Length > MaxSize) throw Bad($"longer than {MaxSize}");
        if (BitConverter.ToUInt32(b, 0) != Magic) throw Bad("magic");
        if (BitConverter.ToUInt16(b, 4) != Version) throw Bad("version");
        ushort flags = BitConverter.ToUInt16(b, 6);
        if ((flags & ~FlagRtl) != 0) throw Bad("unknown flags");
        int tl = 0;
        while (tl < TagBytes && b[8 + tl] != 0) tl++;
        if (tl == 0 || tl == TagBytes) throw Bad("the tag");
        for (int k = tl; k < TagBytes; k++) if (b[8 + k] != 0) throw Bad("the tag's padding");
        uint count = BitConverter.ToUInt32(b, 24), index = BitConverter.ToUInt32(b, 28);
        uint plurals = BitConverter.ToUInt32(b, 32), size = BitConverter.ToUInt32(b, 36);
        if (size != b.Length) throw Bad("its size is not its length");
        if (index != HeaderSize) throw Bad("the index is not after the header");
        if (count > (size - HeaderSize) / RecordSize) throw Bad("more records than fit");
        long dataStart = HeaderSize + (long)RecordSize * count;
        if (plurals < dataStart || plurals > size) throw Bad("the plural rules are out of place");
        var bodies = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        byte[]? prev = null;
        for (int r = 0; r < count; r++)
        {
            int at = HeaderSize + RecordSize * r;
            uint ko = BitConverter.ToUInt32(b, at), kl = BitConverter.ToUInt16(b, at + 4);
            uint bl = BitConverter.ToUInt16(b, at + 6), bo = BitConverter.ToUInt32(b, at + 8);
            if (kl == 0 || kl > MaxKey || ko < dataStart || ko + kl > plurals) throw Bad($"record {r}'s key");
            if (bo < dataStart || bo + bl > plurals) throw Bad($"record {r}'s body");
            byte[] key = b.AsSpan((int)ko, (int)kl).ToArray();
            if (prev is not null && Compare(prev, key) >= 0) throw Bad($"record {r}: keys not strictly rising");
            prev = key;
            byte[] body = b.AsSpan((int)bo, (int)bl).ToArray();
            CheckPattern(body, 0, body.Length, 1, Bad);
            if (!Utf8Ok(key, 0, key.Length)) throw Bad($"record {r}'s key is not UTF-8");
            bodies[Encoding.UTF8.GetString(key)] = body;
        }
        byte[] pb = b.AsSpan((int)plurals, (int)(size - plurals)).ToArray();
        CheckPlurals(pb, Bad);
        return new ReadTable(Encoding.ASCII.GetString(b, 8, tl), (flags & FlagRtl) != 0, bodies, pb);
    }

    private static int Compare(byte[] a, byte[] b)
    {
        int n = Math.Min(a.Length, b.Length);
        for (int k = 0; k < n; k++) if (a[k] != b[k]) return a[k].CompareTo(b[k]);
        return a.Length.CompareTo(b.Length);
    }

    private static void CheckPattern(byte[] p, int at, int end, int depth, Func<string, InvalidDataException> bad)
    {
        if (depth > MaxDepth) throw bad("nested too deep");
        while (at < end)
        {
            byte op = p[at++];
            if (op == OpText)
            {
                if (at + 2 > end) throw bad("a text's length");
                int n = p[at] | (p[at + 1] << 8); at += 2;
                if (at + n > end || !Utf8Ok(p, at, n)) throw bad("a text");
                at += n;
            }
            else if (op == OpVar || op == OpSelect)
            {
                if (at >= end) throw bad("a name's length");
                int n = p[at++];
                if (n == 0 || n > MaxName || at + n > end) throw bad("a name");
                at += n;
                if (op == OpVar) continue;
                if (at >= end) throw bad("a select's count");
                int count = p[at++];
                int defaults = 0;
                for (int v = 0; v < count; v++)
                {
                    if (at >= end) throw bad("a variant");
                    byte kind = p[at++];
                    if (kind == 0) { if (at >= end || p[at] > Plurals.Other) throw bad("a category"); at++; }
                    else if (kind == 1) { if (at + 4 > end) throw bad("an integer key"); at += 4; }
                    else throw bad("a variant's kind");
                    if (at + 3 > end || p[at] > 1) throw bad("a variant's default");
                    defaults += p[at];
                    int bl = p[at + 1] | (p[at + 2] << 8); at += 3;
                    if (at + bl > end) throw bad("a variant's length");
                    CheckPattern(p, at, at + bl, depth + 1, bad);
                    at += bl;
                }
                if (defaults != 1) throw bad("a select without exactly one default");
            }
            else throw bad($"unknown op {op}");
        }
    }

    private static void CheckPlurals(byte[] pb, Func<string, InvalidDataException> bad)
    {
        if (pb.Length < 1) throw bad("no plural block");
        int count = pb[0], at = 1;
        for (int r = 0; r < count; r++)
        {
            if (at + 3 > pb.Length || pb[at] >= Plurals.Other) throw bad("a plural rule's category");
            int len = pb[at + 1] | (pb[at + 2] << 8); at += 3;
            if (at + len > pb.Length) throw bad("a plural rule's length");
            CheckProgram(pb, at, at + len, bad);
            at += len;
        }
        if (at != pb.Length) throw bad("bytes after the plural rules");
    }

    private static void CheckProgram(byte[] p, int at, int end, Func<string, InvalidDataException> bad)
    {
        while (at < end)
        {
            if (p[at] == Plurals.OpOr) { at++; continue; }
            if (p[at] != Plurals.OpRelation || at + 8 > end || p[at + 1] > 7 || p[at + 6] > 1) throw bad("a plural relation");
            int n = p[at + 7];
            at += 8;
            if (at + 8 * n > end) throw bad("a plural relation's ranges");
            at += 8 * n;
        }
    }

    public static bool Utf8Ok(byte[] s, int at, int n)
    {
        int end = at + n;
        while (at < end)
        {
            int b = s[at];
            if (b < 0x80) { at++; continue; }
            int len, lo = 0x80, hi = 0xBF;
            if (b >= 0xC2 && b <= 0xDF) len = 2;
            else if (b >= 0xE0 && b <= 0xEF) { len = 3; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
            else if (b >= 0xF0 && b <= 0xF4) { len = 4; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; }
            else return false;
            if (at + len > end) return false;
            for (int k = 1; k < len; k++)
            {
                int c = s[at + k];
                if (k == 1 && (c < lo || c > hi)) return false;
                if (c < 0x80 || c > 0xBF) return false;
            }
            at += len;
        }
        return true;
    }

    // ---- building a table from sources ----

    public sealed record BuildResult(byte[] Bytes, int Messages, List<string> Warnings);

    /// <summary>
    /// The sources of one app's table in one locale, held to the English
    /// (`baseSources`, when given) and the locale's CLDR categories.
    /// </summary>
    public static BuildResult Build(IList<string> sources, string tag, List<Plurals.LocaleRules> cldr,
                                    IList<string>? baseSources, bool pseudo, bool rtl)
    {
        var warnings = new List<string>();
        var msgs = Collect(sources);
        Dictionary<string, Fluent.Message>? english = baseSources is null ? null : Collect(baseSources);
        // the pseudo-locale is English's text with English's rules
        var rules = Plurals.For(cldr, pseudo ? "en" : tag);
        var cats = Plurals.CategoriesOf(rules);

        if (english is not null)
        {
            foreach (var (k, m) in msgs)
            {
                if (!english.TryGetValue(k, out var em))
                    throw new InvalidDataException($"{m.Where}: '{k}' is not in the English");
                var extra = Fluent.Variables(m.Pattern).Except(Fluent.Variables(em.Pattern)).ToList();
                if (extra.Count > 0)
                    throw new InvalidDataException($"{m.Where}: '{k}' uses ${string.Join(", $", extra)}, which the English does not give it");
            }
            foreach (var k in english.Keys.Where(k => !msgs.ContainsKey(k)).OrderBy(k => k, StringComparer.Ordinal))
                warnings.Add($"'{k}' is not translated: it falls back to the English");
        }

        var bodies = new SortedDictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (var (k, m) in msgs)
        {
            foreach (var sel in Fluent.Selects(m.Pattern))
                foreach (var v in sel.Variants)
                    if (!v.IsNumber && !cats.Contains(v.Category))
                        throw new InvalidDataException($"{m.Where}: '{k}' has a [{Plurals.Categories[v.Category]}] variant; {tag} has no such plural category ({string.Join(", ", cats.OrderBy(c => c).Select(c => Plurals.Categories[c]))})");
            var p = pseudo ? Pseudo.Transform(m.Pattern) : m.Pattern;
            bodies[k] = Pattern(p);
        }
        var table = new Table(tag, rtl || pseudo, bodies, Plurals.Block(rules));
        byte[] bytes = Write(table);
        // read back through the checker: the host must not write what CXOS refuses
        var back = Read(bytes);
        if (back.Bodies.Count != bodies.Count) throw new InvalidDataException("the table read back with a different count");
        return new BuildResult(bytes, bodies.Count, warnings);
    }

    private static Dictionary<string, Fluent.Message> Collect(IList<string> sources)
    {
        var all = new Dictionary<string, Fluent.Message>(StringComparer.Ordinal);
        foreach (string src in sources)
            foreach (var m in Fluent.ParseFile(src))
            {
                if (all.TryGetValue(m.Key, out var was))
                    throw new InvalidDataException($"{m.Where}: '{m.Key}' again (first at {was.Where})");
                all[m.Key] = m;
            }
        return all;
    }
}
