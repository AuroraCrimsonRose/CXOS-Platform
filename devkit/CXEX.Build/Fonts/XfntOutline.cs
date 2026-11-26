// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace CXEX.Build.Fonts;

/// <summary>A point of a v2 outline, in em units (the em is <see cref="XfntOutline.Em"/>).</summary>
public readonly record struct OPoint(short X, short Y, bool OnCurve);

/// <summary>One glyph: its advance and its contours. The box is the points' own.</summary>
public sealed class OGlyph
{
    public int Advance;
    public List<List<OPoint>> Contours = new();
    public short XMin, YMin, XMax, YMax;
    public int PointCount { get { int n = 0; foreach (var c in Contours) n += c.Count; return n; } }
}

/// <summary>An outline face, as XFNT v2 holds it.</summary>
public sealed class OutlineFace
{
    public string Face = "";
    public int Weight = 400;
    public bool Italic, Monospace;
    public int Ascender, Descender, LineGap, CapHeight, XHeight, UnderlinePosition, UnderlineThickness;
    /// <summary>Glyph 0 is the missing-character glyph.</summary>
    public List<OGlyph> Glyphs = new();
    /// <summary>Codepoint to glyph, strictly ascending by codepoint.</summary>
    public List<(uint Codepoint, int Glyph)> Map = new();

    public int Lookup(uint cp)
    {
        int lo = 0, hi = Map.Count - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            if (Map[mid].Codepoint == cp) return Map[mid].Glyph;
            if (Map[mid].Codepoint < cp) lo = mid + 1; else hi = mid - 1;
        }
        return 0;
    }
}

/// <summary>
/// XFNT v2: an outline font as fixed records (`docs/system/CX_FONTS.md` §6).
/// The host converts TrueType into it (<see cref="OutlineConvert"/>) so the
/// OS never parses TrueType; the OS reads these records and checks each one.
///
/// Nothing in the file is an offset. Every section's place and size follows
/// from the header's four counts, every index is checked against a running
/// sum, and the file must be exactly as long as the counts say. So the
/// checker is a sequence of comparisons, as v1's and the CXFS superblock's
/// are, and a font that passes it can be drawn without another check.
///
/// <see cref="Read"/> is the checker, in the order the OS's will run, and
/// the host reads back every font it writes through it.
/// </summary>
public static class XfntOutline
{
    public const ushort Version = 2;
    public const int HeaderSize = 64;
    public const int MapRecord = 8, GlyphRecord = 16, ContourRecord = 8, PointRecord = 4;

    /// <summary>
    /// The em, fixed. A power of two makes the rasteriser's scale a shift
    /// (<see cref="OutlineRaster"/>); 4096 keeps a 1000- or 2048-unit
    /// font's points to well under a pixel at any size the OS draws.
    /// </summary>
    public const int Em = 4096;
    /// <summary>Every coordinate and advance lies within ±4 em.</summary>
    public const int Limit = 4 * Em;

    public const byte FlagItalic = 0x01, FlagMonospace = 0x02, FlagsKnown = 0x03;

    public const int MaxGlyphs = 65535;
    public const int MaxMap = 0x110000;
    public const int MaxContours = 1 << 22;
    public const int MaxPoints = 1 << 24;
    public const int MaxContoursPerGlyph = 65535;

    public static long Length(int glyphs, int map, int contours, int points)
        => HeaderSize + (long)map * MapRecord + (long)glyphs * GlyphRecord
         + (long)contours * ContourRecord + (long)points * PointRecord + Pad4(points);

    private static int Pad4(int n) => (n + 3) & ~3;

    /// <summary>The face name field: 16 bytes of printable ASCII, NUL padded.</summary>
    public const int FaceBytes = 16;

    public static byte[] Write(OutlineFace f)
    {
        if (f.Glyphs.Count is < 1 or > MaxGlyphs) throw new ArgumentException("glyph count out of range");
        if (f.Face.Length is < 1 or > FaceBytes) throw new ArgumentException($"face name must be 1-{FaceBytes} characters");
        foreach (char ch in f.Face)
            if (ch is < ' ' or > '~') throw new ArgumentException("face name must be printable ASCII");

        int contours = 0, points = 0;
        foreach (var g in f.Glyphs)
        {
            Box(g);
            contours += g.Contours.Count;
            points += g.PointCount;
        }

        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write(XfntFormat.Magic);                    // 0
        w.Write(Version);                             // 4
        w.Write(XfntFormat.KindVector);               // 6
        w.Write((byte)((f.Italic ? FlagItalic : 0) | (f.Monospace ? FlagMonospace : 0))); // 7
        w.Write((ushort)Em);                          // 8
        w.Write((ushort)f.Weight);                    // 10
        w.Write((short)f.Ascender);                   // 12
        w.Write((short)f.Descender);                  // 14
        w.Write((short)f.LineGap);                    // 16
        w.Write((short)f.CapHeight);                  // 18
        w.Write((short)f.XHeight);                    // 20
        w.Write((short)f.UnderlinePosition);          // 22
        w.Write((short)f.UnderlineThickness);         // 24
        w.Write((ushort)0);                           // 26 reserved
        w.Write((uint)f.Glyphs.Count);                // 28
        w.Write((uint)f.Map.Count);                   // 32
        w.Write((uint)contours);                      // 36
        w.Write((uint)points);                        // 40
        w.Write(0u);                                  // 44 reserved
        var name = new byte[FaceBytes];
        Encoding.ASCII.GetBytes(f.Face, 0, f.Face.Length, name, 0);
        w.Write(name);                                // 48..63

        foreach (var (cp, g) in f.Map) { w.Write(cp); w.Write((uint)g); }

        int ci = 0;
        foreach (var g in f.Glyphs)
        {
            w.Write((ushort)g.Advance);
            w.Write(g.XMin); w.Write(g.YMin); w.Write(g.XMax); w.Write(g.YMax);
            w.Write((ushort)g.Contours.Count);
            w.Write((uint)ci);
            ci += g.Contours.Count;
        }
        int pi = 0;
        foreach (var g in f.Glyphs)
            foreach (var c in g.Contours) { w.Write((uint)pi); w.Write((uint)c.Count); pi += c.Count; }
        foreach (var g in f.Glyphs)
            foreach (var c in g.Contours)
                foreach (var p in c) { w.Write(p.X); w.Write(p.Y); }
        foreach (var g in f.Glyphs)
            foreach (var c in g.Contours)
                foreach (var p in c) w.Write((byte)(p.OnCurve ? 1 : 0));
        for (int i = points; i < Pad4(points); i++) w.Write((byte)0);

        var bytes = ms.ToArray();
        // the host never writes a font the OS would refuse: a refusal here is
        // the writer's bug, not bad input, so it is not InvalidDataException
        try { Read(bytes); }
        catch (InvalidDataException ex)
        { throw new InvalidOperationException($"the writer produced a font the checker refuses: {ex.Message}", ex); }
        return bytes;
    }

    /// <summary>A glyph's box is its points' own: the control box.</summary>
    public static void Box(OGlyph g)
    {
        if (g.Contours.Count == 0) { g.XMin = g.YMin = g.XMax = g.YMax = 0; return; }
        short x0 = short.MaxValue, y0 = short.MaxValue, x1 = short.MinValue, y1 = short.MinValue;
        foreach (var c in g.Contours)
            foreach (var p in c)
            {
                if (p.X < x0) x0 = p.X; if (p.X > x1) x1 = p.X;
                if (p.Y < y0) y0 = p.Y; if (p.Y > y1) y1 = p.Y;
            }
        g.XMin = x0; g.YMin = y0; g.XMax = x1; g.YMax = y1;
    }

    private static int U16(byte[] f, long o) => BitConverter.ToUInt16(f, (int)o);
    private static int S16(byte[] f, long o) => BitConverter.ToInt16(f, (int)o);
    private static long U32(byte[] f, long o) => BitConverter.ToUInt32(f, (int)o);

    private static void Bad(string why) => throw new InvalidDataException(why);

    private static bool InLimit(int v) => v >= -Limit && v <= Limit;

    /// <summary>
    /// Check and read a v2 file. Each refusal names the rule it broke; the
    /// OS's checker makes the same comparisons in the same order.
    /// </summary>
    public static OutlineFace Read(byte[] file)
    {
        // ---- the header ----
        if (file.Length < HeaderSize) Bad("shorter than the header");
        if (U32(file, 0) != XfntFormat.Magic) Bad("not an XFNT (bad magic)");
        if (U16(file, 4) != Version) Bad($"not XFNT v2 (version {U16(file, 4)})");
        if (file[6] != XfntFormat.KindVector) Bad($"v2 holds outlines only (kind {file[6]})");
        int flags = file[7];
        if ((flags & ~FlagsKnown) != 0) Bad($"unknown flags 0x{flags:X2}");
        if (U16(file, 8) != Em) Bad($"the em must be {Em} (it is {U16(file, 8)})");
        int weight = U16(file, 10);
        if (weight is < 1 or > 1000) Bad($"weight {weight} out of range");
        int asc = S16(file, 12), desc = S16(file, 14), gap = S16(file, 16);
        int capH = S16(file, 18), xH = S16(file, 20), ulPos = S16(file, 22), ulThick = S16(file, 24);
        if (asc is <= 0 or > Limit) Bad($"ascender {asc} out of range");
        if (desc > 0 || desc < -Limit) Bad($"descender {desc} out of range");
        if (gap is < 0 or > Limit) Bad($"line gap {gap} out of range");
        if (capH is < 0 or > Limit || xH is < 0 or > Limit) Bad("cap or x height out of range");
        if (!InLimit(ulPos) || ulThick is < 0 or > Limit) Bad("underline out of range");
        if (U16(file, 26) != 0 || U32(file, 44) != 0) Bad("a reserved field is not zero");
        long glyphs = U32(file, 28), map = U32(file, 32), contours = U32(file, 36), points = U32(file, 40);
        if (glyphs is < 1 or > MaxGlyphs) Bad($"glyph count {glyphs} out of range");
        if (map > MaxMap) Bad($"map count {map} out of range");
        if (contours > MaxContours) Bad($"contour count {contours} out of range");
        if (points > MaxPoints) Bad($"point count {points} out of range");
        long need = Length((int)glyphs, (int)map, (int)contours, (int)points);
        if (file.Length != need) Bad($"file is {file.Length} bytes, the counts describe {need}");
        int nameLen = 0;
        while (nameLen < FaceBytes && file[48 + nameLen] != 0) nameLen++;
        if (nameLen == 0) Bad("the face name is empty");
        for (int i = 0; i < FaceBytes; i++)
        {
            byte b = file[48 + i];
            if (i < nameLen ? b is < 0x20 or > 0x7E : b != 0) Bad("the face name is not printable ASCII, NUL padded");
        }

        var f = new OutlineFace
        {
            Face = Encoding.ASCII.GetString(file, 48, nameLen),
            Weight = weight, Italic = (flags & FlagItalic) != 0, Monospace = (flags & FlagMonospace) != 0,
            Ascender = asc, Descender = desc, LineGap = gap, CapHeight = capH, XHeight = xH,
            UnderlinePosition = ulPos, UnderlineThickness = ulThick,
        };

        long mapAt = HeaderSize;
        long glyphAt = mapAt + map * MapRecord;
        long contourAt = glyphAt + glyphs * GlyphRecord;
        long pointAt = contourAt + contours * ContourRecord;
        long onAt = pointAt + points * PointRecord;

        // ---- the map: strictly ascending codepoints, each a character, each glyph in the font ----
        long prev = -1;
        for (long i = 0; i < map; i++)
        {
            long cp = U32(file, mapAt + i * MapRecord), g = U32(file, mapAt + i * MapRecord + 4);
            if (cp <= prev) Bad($"map entry {i}: codepoints not strictly ascending");
            if (cp > 0x10FFFF || cp is >= 0xD800 and <= 0xDFFF) Bad($"map entry {i}: U+{cp:X} is not a character");
            if (g >= glyphs) Bad($"map entry {i}: glyph {g} is past the font");
            f.Map.Add(((uint)cp, (int)g));
            prev = cp;
        }

        // ---- glyphs: each one's contours follow the last one's; its box within the limit ----
        var gc = new int[glyphs];
        long cSum = 0;
        for (long i = 0; i < glyphs; i++)
        {
            long r = glyphAt + i * GlyphRecord;
            var g = new OGlyph
            {
                Advance = U16(file, r),
                XMin = (short)S16(file, r + 2), YMin = (short)S16(file, r + 4),
                XMax = (short)S16(file, r + 6), YMax = (short)S16(file, r + 8),
            };
            int nc = U16(file, r + 10);
            long first = U32(file, r + 12);
            if (g.Advance > Limit) Bad($"glyph {i}: advance {g.Advance} out of range");
            if (first != cSum) Bad($"glyph {i}: its first contour is {first}, the last glyph ended at {cSum}");
            if (cSum + nc > contours) Bad($"glyph {i}: its contours run past the table");
            if (nc == 0 ? g.XMin != 0 || g.YMin != 0 || g.XMax != 0 || g.YMax != 0
                        : g.XMin > g.XMax || g.YMin > g.YMax || !InLimit(g.XMin) || !InLimit(g.XMax)
                          || !InLimit(g.YMin) || !InLimit(g.YMax))
                Bad($"glyph {i}: box out of range");
            gc[i] = nc;
            cSum += nc;
            f.Glyphs.Add(g);
        }
        if (cSum != contours) Bad($"the glyphs hold {cSum} contours, the header says {contours}");

        // ---- contours: each one's points follow the last one's; at least two ----
        long pSum = 0;
        var cn = new int[contours];
        for (long i = 0; i < contours; i++)
        {
            long r = contourAt + i * ContourRecord;
            long first = U32(file, r), n = U32(file, r + 4);
            if (first != pSum) Bad($"contour {i}: its first point is {first}, the last contour ended at {pSum}");
            if (n < 2) Bad($"contour {i}: {n} points, fewer than two");
            if (pSum + n > points) Bad($"contour {i}: its points run past the table");
            cn[i] = (int)n;
            pSum += n;
        }
        if (pSum != points) Bad($"the contours hold {pSum} points, the header says {points}");

        // ---- points: inside their glyph's box; on-curve flags 0 or 1, each contour starting
        //      on the curve, never two off-curve points together (so every segment is a line
        //      or one quadratic) ----
        long pi = 0, ci = 0;
        for (int gi = 0; gi < glyphs; gi++)
        {
            var g = f.Glyphs[gi];
            for (int k = 0; k < gc[gi]; k++, ci++)
            {
                var c = new List<OPoint>(cn[ci]);
                bool prevOff = false;
                for (int j = 0; j < cn[ci]; j++, pi++)
                {
                    short x = (short)S16(file, pointAt + pi * PointRecord), y = (short)S16(file, pointAt + pi * PointRecord + 2);
                    byte on = file[onAt + pi];
                    if (x < g.XMin || x > g.XMax || y < g.YMin || y > g.YMax)
                        Bad($"glyph {gi}: point {pi} is outside its box");
                    if (on > 1) Bad($"point {pi}: on-curve flag {on}");
                    if (j == 0 && on == 0) Bad($"contour {ci}: starts off the curve");
                    if (on == 0 && prevOff) Bad($"contour {ci}: two off-curve points together");
                    prevOff = on == 0;
                    c.Add(new OPoint(x, y, on == 1));
                }
                g.Contours.Add(c);
            }
        }
        for (long i = onAt + points; i < file.Length; i++)
            if (file[i] != 0) Bad("the padding after the flags is not zero");
        return f;
    }
}
