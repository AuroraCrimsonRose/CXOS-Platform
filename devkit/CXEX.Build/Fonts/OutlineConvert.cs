// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace CXEX.Build.Fonts;

/// <summary>
/// TrueType to XFNT v2, on the host. Everything the OS would otherwise have
/// to get right is done here:
///
/// - the font is cut to glyph 0 and the glyphs its `cmap` reaches (composites
///   are flattened, so nothing else is needed), in the font's own order;
/// - every coordinate is rescaled to the fixed em of 4096 and rounded once,
///   after composites are placed and implied points recovered;
/// - TrueType's implied on-curve points (the midpoint between two off-curve
///   points) are written out, and each contour is turned to start on the
///   curve, so the OS reads lines and single quadratics only;
/// - contours of fewer than two points, which draw nothing, are dropped, as
///   are repeated on-curve points.
/// </summary>
public static class OutlineConvert
{
    /// <summary>
    /// Words in a family name that name a style, not the face, so a new name
    /// may share them ("CX Sans" beside "IBM Plex Sans").
    /// </summary>
    private static readonly HashSet<string> Generic = new(StringComparer.OrdinalIgnoreCase)
    { "Sans", "Serif", "Mono", "Text", "Display", "Condensed", "Next", "Code", "UI" };

    /// <summary>
    /// A converted font is a Modified Version under the OFL, which forbids a
    /// Reserved Font Name on it; a name usually reserves its family's words.
    /// Refuse any new name that shares a non-generic word with the source's
    /// family. (The licence is still read by hand when a font is vendored.)
    /// </summary>
    public static string? NameClash(string face, string family)
    {
        var mine = face.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        foreach (var word in family.Split(' ', StringSplitOptions.RemoveEmptyEntries))
            if (!Generic.Contains(word) && mine.Any(m => m.Equals(word, StringComparison.OrdinalIgnoreCase)))
                return word;
        return null;
    }

    public static OutlineFace Convert(TrueTypeFont tt, string face)
    {
        double s = (double)XfntOutline.Em / tt.UnitsPerEm;
        short Q(double v, string what)
        {
            double r = Math.Round(v * s, MidpointRounding.AwayFromZero);
            if (r < -XfntOutline.Limit || r > XfntOutline.Limit)
                throw new InvalidDataException($"{what} ({v} units) is past ±4 em");
            return (short)r;
        }
        int M(int v, string what) => Q(v, what);

        var f = new OutlineFace
        {
            Face = face,
            Weight = Math.Clamp(tt.WeightClass, 1, 1000),
            Italic = tt.Italic,
            Monospace = tt.Monospace,
            Ascender = M(tt.Ascender, "ascender"),
            Descender = M(Math.Min(tt.Descender, 0), "descender"),
            LineGap = M(Math.Max(tt.LineGap, 0), "line gap"),
            CapHeight = M(Math.Max(tt.CapHeight, 0), "cap height"),
            XHeight = M(Math.Max(tt.XHeight, 0), "x height"),
            UnderlinePosition = M(tt.UnderlinePosition, "underline position"),
            UnderlineThickness = M(Math.Max(tt.UnderlineThickness, 0), "underline thickness"),
        };

        // the other metrics are clamped to their side of the baseline above;
        // a font whose ascender is not above it is wrong, not to be guessed at
        if (f.Ascender <= 0)
            throw new InvalidDataException($"the font's ascender ({tt.Ascender} units) is not above the baseline");

        // glyph 0 and every mapped glyph, in the font's own order
        var keep = new SortedSet<int> { 0 };
        foreach (var g in tt.Map.Values) keep.Add(g);
        var newId = new Dictionary<int, int>();
        foreach (int g in keep)
        {
            newId[g] = f.Glyphs.Count;
            var (contours, adv) = tt.Outline(g);
            var og = new OGlyph { Advance = Math.Max(0, (int)Q(adv, $"glyph {g}'s advance")) };
            foreach (var c in contours)
            {
                var n = Normalise(c, p => Q(p, $"glyph {g}'s point"));
                if (n is not null) og.Contours.Add(n);
            }
            if (og.Contours.Count > XfntOutline.MaxContoursPerGlyph)
                throw new InvalidDataException($"glyph {g} has more than {XfntOutline.MaxContoursPerGlyph} contours");
            XfntOutline.Box(og);
            f.Glyphs.Add(og);
        }
        foreach (var kv in tt.Map.OrderBy(kv => kv.Key))
            f.Map.Add((kv.Key, newId[kv.Value]));
        return f;
    }

    /// <summary>
    /// One contour, implied points made explicit, starting on the curve,
    /// rounded to em units. Null when it draws nothing.
    /// </summary>
    public static List<OPoint>? Normalise(IReadOnlyList<TrueTypeFont.Point> src, Func<double, short> q)
    {
        if (src.Count < 2) return null;
        var full = new List<TrueTypeFont.Point>(src.Count * 2);
        for (int i = 0; i < src.Count; i++)
        {
            var a = src[i];
            var b = src[(i + 1) % src.Count];
            full.Add(a);
            if (!a.OnCurve && !b.OnCurve)
                full.Add(new TrueTypeFont.Point((a.X + b.X) / 2, (a.Y + b.Y) / 2, true));
        }
        int start = full.FindIndex(p => p.OnCurve);
        if (start < 0) return null; // unreachable with two or more points

        var outp = new List<OPoint>(full.Count);
        for (int k = 0; k < full.Count; k++)
        {
            var p = full[(start + k) % full.Count];
            var o = new OPoint(q(p.X), q(p.Y), p.OnCurve);
            // a repeated on-curve point is a line of no length
            if (o.OnCurve && outp.Count > 0 && outp[^1].OnCurve && outp[^1].X == o.X && outp[^1].Y == o.Y)
                continue;
            outp.Add(o);
        }
        // and the closing one, back onto the first
        while (outp.Count > 1 && outp[^1].OnCurve && outp[^1].X == outp[0].X && outp[^1].Y == outp[0].Y)
            outp.RemoveAt(outp.Count - 1);
        return outp.Count >= 2 ? outp : null;
    }
}
