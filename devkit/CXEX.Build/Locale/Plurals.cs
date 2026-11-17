// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;

namespace CXEX.Build.Locale;

/// <summary>
/// CLDR's cardinal plural rules (vendor/cldr/plurals.xml), compiled to the
/// form a string table carries (system/CX_TOOLKIT.md §10): per category but
/// `other`, its condition as relations joined by `and` and `or`, exactly as
/// CLDR writes them, so the X evaluator (std/locale.xfxn, lc_plural) needs
/// no parser. Numbers are integers on CXOS for now, so a rule is evaluated
/// with n = i = |value| and v = w = f = t = c = e = 0, and only CLDR's
/// integer samples without an exponent are kept for the boot test.
/// </summary>
public static class Plurals
{
    public static readonly string[] Categories = { "zero", "one", "two", "few", "many", "other" };
    public const int Other = 5;
    private const string Operands = "nivwftce";

    public const byte OpRelation = 0x52;    // 'R' operand8 mod32 neg8 count8 (lo32 hi32)*
    public const byte OpOr = 0x4F;          // 'O'

    public sealed record Rule(int Category, byte[] Program, List<uint> Samples);
    public sealed record LocaleRules(string[] Tags, List<Rule> Rules, List<uint> OtherSamples);

    public static int CategoryOf(string name)
    {
        int k = Array.IndexOf(Categories, name);
        if (k < 0) throw new InvalidDataException($"'{name}' is not a CLDR plural category ({string.Join(", ", Categories)})");
        return k;
    }

    /// <summary>Every locale's cardinal rules, in file order.</summary>
    public static List<LocaleRules> Load(string path)
    {
        var doc = XDocument.Load(path);
        var all = new List<LocaleRules>();
        var cardinal = doc.Descendants("plurals").Where(p => (string?)p.Attribute("type") is null or "cardinal").ToList();
        if (cardinal.Count != 1) throw new InvalidDataException($"{path}: expected one <plurals type=\"cardinal\">");
        foreach (var pr in cardinal[0].Elements("pluralRules"))
        {
            string[] tags = ((string?)pr.Attribute("locales") ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
            var rules = new List<Rule>();
            List<uint> other = new();
            foreach (var r in pr.Elements("pluralRule"))
            {
                int cat = CategoryOf((string?)r.Attribute("count") ?? "");
                string text = r.Value;
                int at = text.IndexOf('@');
                string cond = (at < 0 ? text : text[..at]).Trim();
                List<uint> samples = IntegerSamples(at < 0 ? "" : text[at..]);
                if (cat == Other)
                {
                    if (cond.Length != 0) throw new InvalidDataException($"{string.Join(' ', tags)}: 'other' has a condition");
                    other = samples;
                    continue;
                }
                rules.Add(new Rule(cat, Compile(cond), samples));
            }
            all.Add(new LocaleRules(tags, rules, other));
        }
        return all;
    }

    /// <summary>The rules for one tag ("en", "pt-PT" or "pt_PT"); `root`'s when CLDR has none for it.</summary>
    public static LocaleRules For(List<LocaleRules> all, string tag)
    {
        string t = tag.Replace('-', '_');
        foreach (var lr in all) if (lr.Tags.Contains(t, StringComparer.OrdinalIgnoreCase)) return lr;
        int us = t.IndexOf('_');
        if (us > 0) return For(all, t[..us]);
        foreach (var lr in all) if (lr.Tags.Contains("root")) return lr;
        throw new InvalidDataException("plurals.xml has no rules for root");
    }

    /// <summary>The categories a locale uses, `other` always.</summary>
    public static HashSet<int> CategoriesOf(LocaleRules lr)
    {
        var s = new HashSet<int>(lr.Rules.Select(r => r.Category)) { Other };
        return s;
    }

    // "@integer 0~15, 100, 1000, … @decimal 0.0~1.5" -> 0..15, 100, 1000
    private static List<uint> IntegerSamples(string s)
    {
        var list = new List<uint>();
        int at = s.IndexOf("@integer", StringComparison.Ordinal);
        if (at < 0) return list;
        string body = s[(at + "@integer".Length)..];
        int dec = body.IndexOf('@');
        if (dec >= 0) body = body[..dec];
        foreach (string raw in body.Split(','))
        {
            string item = raw.Trim();
            if (item.Length == 0 || item == "…") continue;
            if (item.Contains('c') || item.Contains('e')) continue;   // a compact exponent: e != 0, which CXOS's numbers never have
            int tilde = item.IndexOf('~');
            if (tilde < 0) { list.Add(uint.Parse(item, CultureInfo.InvariantCulture)); continue; }
            uint lo = uint.Parse(item[..tilde], CultureInfo.InvariantCulture);
            uint hi = uint.Parse(item[(tilde + 1)..], CultureInfo.InvariantCulture);
            for (uint v = lo; v <= hi; v++) list.Add(v);
        }
        return list;
    }

    /// <summary>A CLDR condition ("v = 0 and i % 10 = 2..4 or ...") to the table's program.</summary>
    public static byte[] Compile(string cond)
    {
        var o = new List<byte>();
        string[] ors = cond.Split(" or ");
        for (int g = 0; g < ors.Length; g++)
        {
            if (g > 0) o.Add(OpOr);
            foreach (string rel0 in ors[g].Split(" and "))
            {
                string rel = rel0.Trim();
                bool neg = rel.Contains("!=");
                string[] sides = rel.Split(neg ? "!=" : "=");
                if (sides.Length != 2) throw new InvalidDataException($"plural relation '{rel}': expected 'expr = ranges'");
                string lhs = sides[0].Trim();
                uint mod = 0;
                int pct = lhs.IndexOf('%');
                if (pct >= 0)
                {
                    mod = uint.Parse(lhs[(pct + 1)..].Trim(), CultureInfo.InvariantCulture);
                    if (mod == 0) throw new InvalidDataException($"plural relation '{rel}': modulus 0");
                    lhs = lhs[..pct].Trim();
                }
                if (lhs.Length != 1 || Operands.IndexOf(lhs[0]) < 0)
                    throw new InvalidDataException($"plural relation '{rel}': operand '{lhs}' is not one of {Operands}");
                var ranges = new List<(uint, uint)>();
                foreach (string r0 in sides[1].Split(','))
                {
                    string r = r0.Trim();
                    int dots = r.IndexOf("..", StringComparison.Ordinal);
                    uint lo = uint.Parse(dots < 0 ? r : r[..dots], CultureInfo.InvariantCulture);
                    uint hi = dots < 0 ? lo : uint.Parse(r[(dots + 2)..], CultureInfo.InvariantCulture);
                    if (hi < lo) throw new InvalidDataException($"plural relation '{rel}': range {lo}..{hi}");
                    ranges.Add((lo, hi));
                }
                if (ranges.Count > 255) throw new InvalidDataException($"plural relation '{rel}': too many ranges");
                o.Add(OpRelation);
                o.Add((byte)Operands.IndexOf(lhs[0]));
                U32(o, mod);
                o.Add(neg ? (byte)1 : (byte)0);
                o.Add((byte)ranges.Count);
                foreach (var (lo, hi) in ranges) { U32(o, lo); U32(o, hi); }
            }
        }
        return o.ToArray();
    }

    private static void U32(List<byte> o, uint v)
    {
        o.Add((byte)v); o.Add((byte)(v >> 8)); o.Add((byte)(v >> 16)); o.Add((byte)(v >> 24));
    }

    /// <summary>
    /// What the X evaluator does, for the host's own checks: the category of an
    /// integer under a locale's rules (the first that holds, else other).
    /// </summary>
    public static int Evaluate(LocaleRules lr, ulong value)
    {
        foreach (var r in lr.Rules) if (Holds(r.Program, value)) return r.Category;
        return Other;
    }

    public static bool Holds(byte[] p, ulong value)
    {
        bool any = false, group = true;
        int at = 0;
        while (at < p.Length)
        {
            if (p[at] == OpOr) { any |= group; group = true; at++; continue; }
            int operand = p[at + 1];
            uint mod = BitConverter.ToUInt32(p, at + 2);
            bool neg = p[at + 6] != 0;
            int count = p[at + 7];
            at += 8;
            ulong x = operand <= 1 ? value : 0;     // n and i are the value; the rest 0
            if (mod != 0) x %= mod;
            bool inside = false;
            for (int k = 0; k < count; k++, at += 8)
            {
                uint lo = BitConverter.ToUInt32(p, at), hi = BitConverter.ToUInt32(p, at + 4);
                if (x >= lo && x <= hi) inside = true;
            }
            group &= neg ? !inside : inside;
        }
        return any | group;
    }

    /// <summary>The rule block a table carries: count8, then per rule cat8 len16 program.</summary>
    public static byte[] Block(LocaleRules lr)
    {
        var o = new List<byte> { (byte)lr.Rules.Count };
        foreach (var r in lr.Rules)
        {
            if (r.Program.Length > 0xFFFF) throw new InvalidDataException("a plural rule longer than 65535 bytes");
            o.Add((byte)r.Category);
            o.Add((byte)r.Program.Length); o.Add((byte)(r.Program.Length >> 8));
            o.AddRange(r.Program);
        }
        return o.ToArray();
    }

    // ---- XPLT: every locale's rules and CLDR's samples, for the boot test ----

    public const uint XpltMagic = 0x544C5058;   // 'X','P','L','T'
    public const int TagBytes = 16;

    /// <summary>
    /// `XPLT` version16 (XSTR's: it carries XSTR's rule blocks) 0 count32 size32,
    /// then per locale: tag[16], len16 and the rule block, count16 and that many
    /// samples (value32, category8). One record a tag, so a locale's own
    /// rules are what is tested under its own name.
    /// </summary>
    public static byte[] Xplt(List<LocaleRules> all)
    {
        var o = new List<byte>();
        int records = 0;
        foreach (var lr in all)
        {
            byte[] block = Block(lr);
            var samples = new List<(uint, int)>();
            foreach (var r in lr.Rules) foreach (uint v in r.Samples) samples.Add((v, r.Category));
            foreach (uint v in lr.OtherSamples) samples.Add((v, Other));
            // CLDR's samples hold under CLDR's rules, or the conversion is wrong
            foreach (var (v, c) in samples)
                if (Evaluate(lr, v) != c)
                    throw new InvalidDataException($"{lr.Tags[0]}: CLDR's sample {v} is {Categories[c]}, the compiled rules say {Categories[Evaluate(lr, v)]}");
            foreach (string tag in lr.Tags)
            {
                if (tag.Length >= TagBytes) throw new InvalidDataException($"locale tag '{tag}' is {tag.Length} characters; at most {TagBytes - 1}");
                for (int k = 0; k < TagBytes; k++) o.Add(k < tag.Length ? (byte)tag[k] : (byte)0);
                o.Add((byte)block.Length); o.Add((byte)(block.Length >> 8));
                o.AddRange(block);
                o.Add((byte)samples.Count); o.Add((byte)(samples.Count >> 8));
                foreach (var (v, c) in samples) { U32(o, v); o.Add((byte)c); }
                records++;
            }
        }
        var h = new List<byte>();
        U32(h, XpltMagic);
        h.Add((byte)Xstr.Version); h.Add((byte)(Xstr.Version >> 8));
        h.Add(0); h.Add(0);
        U32(h, (uint)records);
        U32(h, (uint)(16 + o.Count));
        h.AddRange(o);
        return h.ToArray();
    }
}
