// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace CXEX.Build.Fonts;

/// <summary>
/// A TrueType (glyf) font read on the host, so that the OS never parses one
/// (roadmap 2.8a′, Aurora 2026-11-11). It reads only what XFNT v2 carries: the
/// em, the metrics, the advances, the codepoint map and every glyph's
/// outline, with composite glyphs flattened into plain contours.
///
/// Every read is bounds-checked and a bad font is an
/// <see cref="InvalidDataException"/> naming the table: a parser bug here
/// costs a build, not a machine, but it should still say where.
///
/// Not read: hinting (dropped by design), kerning and GSUB/GPOS (later
/// steps), variable fonts (instanced on the host when one is needed) and CFF
/// outlines (OpenType's cubic flavour: refused for now, with the reason).
/// The composite rules follow the OpenType spec's `glyf` chapter;
/// SerenityOS's `LibGfx/Font/OpenType/Glyf.cpp` was read for how the implied
/// on-curve points are recovered, which <see cref="OutlineFont"/> does.
/// </summary>
public sealed class TrueTypeFont
{
    public int UnitsPerEm { get; private set; }
    public int Ascender { get; private set; }
    public int Descender { get; private set; }
    public int LineGap { get; private set; }
    public int CapHeight { get; private set; }
    public int XHeight { get; private set; }
    public int UnderlinePosition { get; private set; }
    public int UnderlineThickness { get; private set; }
    public int WeightClass { get; private set; }
    public bool Italic { get; private set; }
    public bool Monospace { get; private set; }
    public string Family { get; private set; } = "";
    public int GlyphCount { get; private set; }

    /// <summary>Codepoint to glyph, from the best Unicode `cmap` subtable.</summary>
    public IReadOnlyDictionary<uint, int> Map => _map;

    private readonly byte[] _f;
    private readonly Dictionary<string, (int Off, int Len)> _tables = new();
    private readonly Dictionary<uint, int> _map = new();
    private int[] _advance = Array.Empty<int>();
    private int[] _loca = Array.Empty<int>();
    private int _glyfOff, _glyfLen;

    private TrueTypeFont(byte[] f) { _f = f; }

    public static TrueTypeFont Load(string path) => Parse(File.ReadAllBytes(path));

    public static TrueTypeFont Parse(byte[] f)
    {
        return new TrueTypeFont(f).Read();
    }

    // ---- bounds-checked big-endian reads ----

    private void Need(int off, int len, string what)
    {
        if (off < 0 || len < 0 || (long)off + len > _f.Length)
            throw new InvalidDataException($"{what}: reads past the end of the file");
    }
    private int U8(int o) { Need(o, 1, "font"); return _f[o]; }
    private int U16(int o) { Need(o, 2, "font"); return (_f[o] << 8) | _f[o + 1]; }
    private int S16(int o) => (short)U16(o);
    private int I32(int o)
    {
        uint v = U32(o);
        if (v > int.MaxValue) throw new InvalidDataException($"a 32-bit field at {o} is past any file's length");
        return (int)v;
    }
    private uint U32(int o) { Need(o, 4, "font"); return ((uint)_f[o] << 24) | ((uint)_f[o + 1] << 16) | ((uint)_f[o + 2] << 8) | _f[o + 3]; }

    private (int Off, int Len) Table(string tag, bool required = true)
    {
        if (_tables.TryGetValue(tag, out var t)) return t;
        if (required) throw new InvalidDataException($"the font has no '{tag}' table");
        return (-1, 0);
    }

    private TrueTypeFont Read()
    {
        uint sfnt = U32(0);
        if (sfnt == 0x4F54544F) // 'OTTO'
            throw new InvalidDataException("this is a CFF (cubic) OpenType font; only TrueType (glyf) outlines are converted so far");
        if (sfnt != 0x00010000 && sfnt != 0x74727565) // 1.0 or 'true'
            throw new InvalidDataException($"not a TrueType font (sfnt version 0x{sfnt:X8})");
        int n = U16(4);
        for (int i = 0; i < n; i++)
        {
            int r = 12 + i * 16;
            Need(r, 16, "the table directory");
            string tag = Encoding.ASCII.GetString(_f, r, 4);
            int off = I32(r + 8), len = I32(r + 12);
            Need(off, len, $"table '{tag}'");
            _tables[tag] = (off, len);
        }

        var head = Table("head");
        if (head.Len < 54) throw new InvalidDataException("'head' is too short");
        int upm = U16(head.Off + 18);
        if (upm is < 16 or > 16384) throw new InvalidDataException($"'head' unitsPerEm {upm} out of range");
        int locaFormat = S16(head.Off + 50);

        var maxp = Table("maxp");
        int glyphs = U16(maxp.Off + 4);
        if (glyphs < 1) throw new InvalidDataException("'maxp' says the font has no glyphs");

        var hhea = Table("hhea");
        int asc = S16(hhea.Off + 4), desc = S16(hhea.Off + 6), gap = S16(hhea.Off + 8);
        int nHm = U16(hhea.Off + 34);
        if (nHm < 1 || nHm > glyphs) throw new InvalidDataException($"'hhea' numberOfHMetrics {nHm} out of range");

        var hmtx = Table("hmtx");
        Need(hmtx.Off, nHm * 4, "'hmtx'");
        _advance = new int[glyphs];
        for (int g = 0; g < glyphs; g++)
            _advance[g] = U16(hmtx.Off + Math.Min(g, nHm - 1) * 4);

        var loca = Table("loca");
        _loca = new int[glyphs + 1];
        for (int g = 0; g <= glyphs; g++)
            _loca[g] = locaFormat == 0 ? U16(loca.Off + g * 2) * 2 : I32(loca.Off + g * 4);
        (_glyfOff, _glyfLen) = Table("glyf");
        for (int g = 0; g < glyphs; g++)
            if (_loca[g] > _loca[g + 1] || _loca[g + 1] > _glyfLen)
                throw new InvalidDataException($"'loca' entry {g} is out of order or past 'glyf'");

        int capH = 0, xH = 0, weight = 400;
        bool italic = false;
        var os2 = Table("OS/2", required: false);
        if (os2.Off >= 0)
        {
            int ver = U16(os2.Off);
            weight = U16(os2.Off + 4);
            italic = (U16(os2.Off + 62) & 1) != 0;
            if ((U16(os2.Off + 62) & 0x80) != 0) // USE_TYPO_METRICS
            {
                asc = S16(os2.Off + 68); desc = S16(os2.Off + 70); gap = S16(os2.Off + 72);
            }
            if (ver >= 2 && os2.Len >= 90) { xH = S16(os2.Off + 86); capH = S16(os2.Off + 88); }
        }
        else italic = (U16(head.Off + 44) & 2) != 0;

        int ulPos = 0, ulThick = 0;
        bool mono = false;
        var post = Table("post", required: false);
        if (post.Off >= 0 && post.Len >= 16)
        {
            ulPos = S16(post.Off + 8); ulThick = S16(post.Off + 10);
            mono = U32(post.Off + 12) != 0;
        }

        UnitsPerEm = upm; Ascender = asc; Descender = desc; LineGap = gap;
        CapHeight = capH; XHeight = xH; UnderlinePosition = ulPos; UnderlineThickness = ulThick;
        WeightClass = weight; Italic = italic; Monospace = mono;
        Family = ReadFamily(); GlyphCount = glyphs;
        ReadCmap();
        return this;
    }

    private string ReadFamily()
    {
        var name = Table("name", required: false);
        if (name.Off < 0) return "";
        int count = U16(name.Off + 2), strings = name.Off + U16(name.Off + 4);
        string? best = null;
        for (int i = 0; i < count; i++)
        {
            int r = name.Off + 6 + i * 12;
            int plat = U16(r), enc = U16(r + 2), id = U16(r + 6), len = U16(r + 8), off = U16(r + 10);
            if (id != 1 && id != 16) continue;
            Need(strings + off, len, "'name'");
            string s = plat == 3 || plat == 0
                ? Encoding.BigEndianUnicode.GetString(_f, strings + off, len)
                : Encoding.Latin1.GetString(_f, strings + off, len);
            if (id == 16 || best is null) best = s; // the typographic family wins
            _ = enc;
        }
        return best ?? "";
    }

    private void ReadCmap()
    {
        var cmap = Table("cmap");
        int n = U16(cmap.Off + 2);
        int best = -1, bestRank = 0;
        for (int i = 0; i < n; i++)
        {
            int r = cmap.Off + 4 + i * 8;
            int plat = U16(r), enc = U16(r + 2);
            long off64 = cmap.Off + (long)U32(r + 4);
            if (off64 > _f.Length - 2) throw new InvalidDataException("'cmap': a subtable past the end of the file");
            int off = (int)off64;
            int fmt = U16(off);
            int rank = (plat, enc, fmt) switch
            {
                (3, 10, 12) => 4,
                (0, 4, 12) or (0, 6, 12) => 3,
                (3, 1, 4) => 2,
                (0, _, 4) => 1,
                _ => 0,
            };
            if (rank > bestRank) { bestRank = rank; best = off; }
        }
        if (best < 0) throw new InvalidDataException("'cmap' has no Unicode subtable of format 4 or 12");

        if (U16(best) == 12)
        {
            uint groups = U32(best + 12);
            if (groups > 0x110000) throw new InvalidDataException("'cmap' format 12 has too many groups");
            for (int g = 0; g < groups; g++)
            {
                int r = best + 16 + g * 12;
                uint start = U32(r), end = U32(r + 4), gid = U32(r + 8);
                if (end < start || end > 0x10FFFF) throw new InvalidDataException("'cmap' format 12 group out of range");
                for (uint c = start; c <= end; c++) Add(c, (long)gid + (c - start));
            }
        }
        else
        {
            int segX2 = U16(best + 6);
            int ends = best + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
            for (int s = 0; s < segX2 / 2; s++)
            {
                int end = U16(ends + s * 2), start = U16(starts + s * 2);
                int delta = S16(deltas + s * 2), roff = U16(ranges + s * 2);
                if (start > end) continue;
                for (int c = start; c <= end && c != 0xFFFF; c++)
                {
                    int gid;
                    if (roff == 0) gid = (c + delta) & 0xFFFF;
                    else
                    {
                        int at = ranges + s * 2 + roff + (c - start) * 2;
                        gid = U16(at);
                        if (gid != 0) gid = (gid + delta) & 0xFFFF;
                    }
                    Add((uint)c, gid);
                }
            }
        }
    }

    private void Add(uint cp, long gid)
    {
        if (gid <= 0 || gid >= GlyphCount) return;            // unmapped, or past the font
        if (cp is >= 0xD800 and <= 0xDFFF) return;            // surrogates are not characters
        _map.TryAdd(cp, (int)gid);
    }

    public int Advance(int glyph) => _advance[glyph];

    /// <summary>A point of an outline, in font units, as the font draws it.</summary>
    public readonly record struct Point(double X, double Y, bool OnCurve);

    /// <summary>
    /// A glyph's contours in font units, composites flattened. A composite's
    /// USE_MY_METRICS advance is returned too.
    /// </summary>
    public (List<List<Point>> Contours, int Advance) Outline(int glyph)
    {
        var contours = new List<List<Point>>();
        int adv = _advance[glyph];
        Append(glyph, contours, 1, 0, 0, 1, 0, 0, 0, ref adv);
        return (contours, adv);
    }

    private void Append(int g, List<List<Point>> into, double a, double b, double c, double d,
                        double dx, double dy, int depth, ref int advance)
    {
        if (depth > 8) throw new InvalidDataException($"glyph {g}: composites nest deeper than 8 (a cycle?)");
        int start = _loca[g], len = _loca[g + 1] - start;
        if (len == 0) return;
        int o = _glyfOff + start;
        Need(o, Math.Min(len, 10), $"glyph {g}");
        int nc = S16(o);
        int end = o + len;
        if (nc >= 0) { AppendSimple(g, o, end, nc, into, a, b, c, d, dx, dy); return; }

        int p = o + 10;
        while (true)
        {
            int flags = U16(p), comp = U16(p + 2);
            p += 4;
            if (comp >= GlyphCount) throw new InvalidDataException($"glyph {g}: component {comp} is past the font");
            int e, f;
            if ((flags & 0x0001) != 0) { e = S16(p); f = S16(p + 2); p += 4; }
            else { e = (sbyte)U8(p); f = (sbyte)U8(p + 1); p += 2; }
            if ((flags & 0x0002) == 0)
                throw new InvalidDataException($"glyph {g}: a component placed by matching points is not supported");
            double ca = 1, cb = 0, cc = 0, cd = 1;
            if ((flags & 0x0008) != 0) { ca = cd = F2Dot14(p); p += 2; }
            else if ((flags & 0x0040) != 0) { ca = F2Dot14(p); cd = F2Dot14(p + 2); p += 4; }
            else if ((flags & 0x0080) != 0) { ca = F2Dot14(p); cb = F2Dot14(p + 2); cc = F2Dot14(p + 4); cd = F2Dot14(p + 6); p += 8; }
            if (p > end) throw new InvalidDataException($"glyph {g}: the composite runs past its own length");

            double ox = e, oy = f;
            if ((flags & 0x0800) != 0 && (flags & 0x1000) == 0) // SCALED_COMPONENT_OFFSET
            { ox = ca * e + cc * f; oy = cb * e + cd * f; }
            if ((flags & 0x0004) != 0) { ox = Math.Round(ox); oy = Math.Round(oy); } // ROUND_XY_TO_GRID
            if ((flags & 0x0200) != 0) advance = _advance[comp];                     // USE_MY_METRICS

            // child point q maps to (ca*qx + cc*qy + ox, cb*qx + cd*qy + oy), then the parent's transform
            double na = a * ca + c * cb, nb = b * ca + d * cb;
            double nc2 = a * cc + c * cd, nd = b * cc + d * cd;
            double ndx = a * ox + c * oy + dx, ndy = b * ox + d * oy + dy;
            int dummy = 0;
            Append(comp, into, na, nb, nc2, nd, ndx, ndy, depth + 1, ref dummy);

            if ((flags & 0x0020) == 0) break; // MORE_COMPONENTS
        }
    }

    private double F2Dot14(int o) => S16(o) / 16384.0;

    private void AppendSimple(int g, int o, int end, int nc, List<List<Point>> into,
                              double a, double b, double c, double d, double dx, double dy)
    {
        if (nc == 0) return;
        var endPts = new int[nc];
        for (int i = 0; i < nc; i++)
        {
            endPts[i] = U16(o + 10 + i * 2);
            if (i > 0 && endPts[i] <= endPts[i - 1])
                throw new InvalidDataException($"glyph {g}: contour ends are not increasing");
        }
        int nPts = endPts[nc - 1] + 1;
        int p = o + 10 + nc * 2;
        int nIns = U16(p);
        p += 2 + nIns;

        var flags = new byte[nPts];
        for (int i = 0; i < nPts;)
        {
            if (p >= end) throw new InvalidDataException($"glyph {g}: flags run past the glyph");
            byte fl = (byte)U8(p++);
            flags[i++] = fl;
            if ((fl & 8) != 0)
            {
                if (p >= end) throw new InvalidDataException($"glyph {g}: flags run past the glyph");
                int rep = U8(p++);
                for (int k = 0; k < rep; k++)
                {
                    if (i >= nPts) throw new InvalidDataException($"glyph {g}: a flag repeats past the last point");
                    flags[i++] = fl;
                }
            }
        }
        var xs = new int[nPts];
        var ys = new int[nPts];
        int v = 0;
        for (int i = 0; i < nPts; i++)
        {
            byte fl = flags[i];
            if ((fl & 2) != 0) { int dv = U8(p++); v += (fl & 16) != 0 ? dv : -dv; }
            else if ((fl & 16) == 0) { v += S16(p); p += 2; }
            xs[i] = v;
        }
        v = 0;
        for (int i = 0; i < nPts; i++)
        {
            byte fl = flags[i];
            if ((fl & 4) != 0) { int dv = U8(p++); v += (fl & 32) != 0 ? dv : -dv; }
            else if ((fl & 32) == 0) { v += S16(p); p += 2; }
            ys[i] = v;
        }
        if (p > end) throw new InvalidDataException($"glyph {g}: coordinates run past the glyph");

        int first = 0;
        for (int ci = 0; ci < nc; ci++)
        {
            var contour = new List<Point>();
            for (int i = first; i <= endPts[ci]; i++)
                contour.Add(new Point(a * xs[i] + c * ys[i] + dx, b * xs[i] + d * ys[i] + dy, (flags[i] & 1) != 0));
            into.Add(contour);
            first = endPts[ci] + 1;
        }
    }
}
