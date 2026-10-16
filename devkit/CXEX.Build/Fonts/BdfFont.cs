// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace CXEX.Build.Fonts;

/// <summary>
/// A Glyph Bitmap Distribution Format (BDF 2.1) font: the plain-text source
/// format bitmap fonts such as Terminus and GNU Unifont are published in.
///
/// <para>Only what decides a glyph's pixels and its place in the cell is read:
/// the font bounding box, the ascent, and per glyph its encoding (a Unicode
/// codepoint in an ISO 10646 font), its BBX and its BITMAP rows. Properties
/// beyond those are skipped rather than half-supported.</para>
///
/// <para>Read on the host only. The kernel never sees BDF; `cxk font build`
/// turns it into XFNT, the fixed layout the kernel reads.</para>
/// </summary>
public sealed class BdfFont
{
    /// <summary>The cell: FONTBOUNDINGBOX's width and height.</summary>
    public int CellWidth { get; private init; }
    public int CellHeight { get; private init; }

    /// <summary>FONTBOUNDINGBOX's offsets: where the cell sits against the origin.</summary>
    public int CellXOffset { get; private init; }
    public int CellYOffset { get; private init; }

    /// <summary>The FONT line, the XLFD name.</summary>
    public string Name { get; private init; } = "";

    /// <summary>FAMILY_NAME when present, else the XLFD family field.</summary>
    public string Family { get; private init; } = "";

    /// <summary>Glyphs by codepoint, each already placed in the cell: one
    /// MSB-first row per scanline, <see cref="CellHeight"/> rows.</summary>
    public IReadOnlyDictionary<uint, byte[][]> Glyphs => _glyphs;
    private readonly Dictionary<uint, byte[][]> _glyphs = new();

    public static BdfFont Load(string path) => Parse(File.ReadAllText(path));

    public static BdfFont Parse(string text)
    {
        var lines = text.Replace("\r\n", "\n").Split('\n');
        int i = 0;

        string Next() => i < lines.Length ? lines[i++].Trim() : throw new InvalidDataException("BDF ends early");

        if (!Next().StartsWith("STARTFONT ", StringComparison.Ordinal))
            throw new InvalidDataException("not a BDF font: the first line is not STARTFONT");

        int cw = 0, ch = 0, cx = 0, cy = 0;
        string name = "", family = "";
        bool haveBox = false;
        var glyphs = new Dictionary<uint, byte[][]>();

        while (i < lines.Length)
        {
            string line = Next();
            if (line.Length == 0 || line.StartsWith("COMMENT", StringComparison.Ordinal)) continue;
            string[] f = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);

            switch (f[0])
            {
                case "FONT":
                    name = line.Length > 5 ? line[5..].Trim() : "";
                    break;
                case "FAMILY_NAME":
                    family = line[11..].Trim().Trim('"');
                    break;
                case "FONTBOUNDINGBOX":
                    cw = Int(f, 1); ch = Int(f, 2); cx = Int(f, 3); cy = Int(f, 4);
                    if (cw < 1 || ch < 1) throw new InvalidDataException($"FONTBOUNDINGBOX {cw}x{ch} is empty");
                    haveBox = true;
                    break;
                case "STARTCHAR":
                    if (!haveBox) throw new InvalidDataException("a glyph comes before FONTBOUNDINGBOX");
                    var (cp, rows) = ReadChar(Next, cw, ch, cx, cy);
                    // ENCODING -1 means "unencoded" in BDF: no codepoint, nothing to place.
                    if (cp is uint c)
                    {
                        if (glyphs.ContainsKey(c))
                            throw new InvalidDataException($"U+{c:X4} is defined twice");
                        glyphs[c] = rows;
                    }
                    break;
                case "ENDFONT":
                    i = lines.Length;
                    break;
            }
        }

        if (!haveBox) throw new InvalidDataException("no FONTBOUNDINGBOX");

        if (family.Length == 0)
        {
            // XLFD: -FOUNDRY-FAMILY-WEIGHT-...; the family is the second field.
            var x = name.Split('-');
            if (x.Length > 2) family = x[2];
        }

        var font = new BdfFont
        {
            CellWidth = cw, CellHeight = ch, CellXOffset = cx, CellYOffset = cy,
            Name = name, Family = family,
        };
        foreach (var kv in glyphs) font._glyphs[kv.Key] = kv.Value;
        return font;
    }

    /// <summary>
    /// One STARTCHAR..ENDCHAR block, its bitmap placed in the cell.
    ///
    /// <para>BDF gives each glyph its own box (BBX w h x y, relative to the
    /// origin on the baseline) and the font one box for all of them
    /// (FONTBOUNDINGBOX). Placing a glyph is the difference of the two: its
    /// left edge is x - cellX columns in, and its top row is
    /// (cellY + cellH) - (y + h) rows down. A glyph that falls outside the cell
    /// is refused by name, not cropped: a cropped glyph is a wrong glyph that
    /// nothing reports.</para>
    /// </summary>
    private static (uint? cp, byte[][] rows) ReadChar(Func<string> next, int cw, int ch, int cx, int cy)
    {
        uint? cp = null;
        int bw = 0, bh = 0, bx = 0, by = 0;
        bool haveBbx = false, haveEnc = false;
        string line;

        while ((line = next()) != "BITMAP")
        {
            string[] f = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (f.Length == 0) continue;
            switch (f[0])
            {
                case "ENCODING":
                    int e = Int(f, 1);
                    haveEnc = true;
                    if (e >= 0) cp = (uint)e;
                    break;
                case "BBX":
                    bw = Int(f, 1); bh = Int(f, 2); bx = Int(f, 3); by = Int(f, 4);
                    haveBbx = true;
                    break;
                case "ENDCHAR":
                    throw new InvalidDataException("a glyph has no BITMAP");
            }
        }
        if (!haveEnc) throw new InvalidDataException("a glyph has no ENCODING");
        if (!haveBbx) throw new InvalidDataException($"{Describe(cp)} has no BBX");

        int left = bx - cx;
        int top = (cy + ch) - (by + bh);
        if (bw < 0 || bh < 0 || left < 0 || top < 0 || left + bw > cw || top + bh > ch)
            throw new InvalidDataException(
                $"{Describe(cp)}: its box {bw}x{bh} at ({bx},{by}) does not fit the {cw}x{ch} cell");

        int bytesPerRow = (cw + 7) / 8;
        var rows = new byte[ch][];
        for (int r = 0; r < ch; r++) rows[r] = new byte[bytesPerRow];

        int srcBytes = (bw + 7) / 8;
        for (int r = 0; r < bh; r++)
        {
            string hex = next();
            if (hex == "ENDCHAR")
                throw new InvalidDataException($"{Describe(cp)}: BITMAP has {r} rows, BBX says {bh}");
            if (hex.Length < srcBytes * 2)
                throw new InvalidDataException($"{Describe(cp)}: row {r} is {hex.Length} hex digits, needs {srcBytes * 2}");

            for (int col = 0; col < bw; col++)
            {
                int b = Convert.ToInt32(hex.Substring((col / 8) * 2, 2), 16);
                if ((b & (0x80 >> (col % 8))) == 0) continue;
                int x = left + col;
                rows[top + r][x / 8] |= (byte)(0x80 >> (x % 8));
            }
        }

        if (next() != "ENDCHAR")
            throw new InvalidDataException($"{Describe(cp)}: BITMAP has more rows than BBX says ({bh})");

        return (cp, rows);
    }

    private static string Describe(uint? cp) => cp is uint c ? $"U+{c:X4}" : "an unencoded glyph";

    private static int Int(string[] f, int at)
    {
        if (at >= f.Length || !int.TryParse(f[at], NumberStyles.Integer, CultureInfo.InvariantCulture, out int v))
            throw new InvalidDataException($"'{string.Join(' ', f)}': expected a number in field {at}");
        return v;
    }
}
