// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace CXEX.Build.Locale;

/// <summary>
/// The pseudo-locale `qps` (system/CX_TOOLKIT.md §10), made from the
/// English: letters accented, 40% longer, in brackets, and right-to-left,
/// so a string that is not looked up, a label too narrow for a longer
/// translation or a layout that does not mirror is seen in the
/// verification boots. The accents are ones code page 437 has, so the v1
/// bitmap font draws them. Placeables, mnemonic letters (after `&`) and
/// select keys are left as they are.
/// </summary>
public static class Pseudo
{
    public const string Tag = "qps";

    private static readonly Dictionary<char, char> Accents = new()
    {
        ['a'] = 'å', ['c'] = 'ç', ['e'] = 'é', ['i'] = 'ï', ['n'] = 'ñ', ['o'] = 'ö', ['u'] = 'ü', ['y'] = 'ÿ',
        ['A'] = 'Å', ['C'] = 'Ç', ['E'] = 'É', ['N'] = 'Ñ', ['O'] = 'Ö', ['U'] = 'Ü',
    };

    public static string Accent(string s)
    {
        var sb = new StringBuilder(s.Length);
        for (int k = 0; k < s.Length; k++)
        {
            char c = s[k];
            if (c == '&' && k + 1 < s.Length)
            {
                // "&&" is an ampersand; "&x" marks x, which keeps its key
                sb.Append(c).Append(s[k + 1]);
                k++;
                continue;
            }
            sb.Append(Accents.TryGetValue(c, out var a) ? a : c);
        }
        return sb.ToString();
    }

    // The characters the longest way through a pattern shows.
    private static int Length(List<Fluent.Element> p)
    {
        int n = 0;
        foreach (var e in p)
        {
            if (e is Fluent.Text t) n += new StringInfoCount(t.Value).Count;
            else if (e is Fluent.Select s) n += s.Variants.Max(v => Length(v.Pattern));
        }
        return n;
    }

    private static List<Fluent.Element> AccentAll(List<Fluent.Element> p) =>
        p.Select(e => e switch
        {
            Fluent.Text t => (Fluent.Element)new Fluent.Text(Accent(t.Value)),
            Fluent.Select s => new Fluent.Select(s.Name, s.Variants.Select(v => v with { Pattern = AccentAll(v.Pattern) }).ToList()),
            _ => e,
        }).ToList();

    /// <summary>"[" + the accented pattern + "~" for 40% of its length (at least one) + "]".</summary>
    public static List<Fluent.Element> Transform(List<Fluent.Element> p)
    {
        int pad = Math.Max(1, (Length(p) * 4 + 9) / 10);
        var o = new List<Fluent.Element> { new Fluent.Text("[") };
        o.AddRange(AccentAll(p));
        o.Add(new Fluent.Text(new string('~', pad) + "]"));
        // adjacent texts merged, as the parser leaves them
        var m = new List<Fluent.Element>();
        foreach (var e in o)
            if (e is Fluent.Text t && m.Count > 0 && m[^1] is Fluent.Text u) m[^1] = new Fluent.Text(u.Value + t.Value);
            else m.Add(e);
        return m;
    }

    private readonly struct StringInfoCount
    {
        public int Count { get; }
        public StringInfoCount(string s)
        {
            int n = 0;
            for (int k = 0; k < s.Length; k++) { if (char.IsHighSurrogate(s[k])) k++; n++; }
            Count = n;
        }
    }
}
