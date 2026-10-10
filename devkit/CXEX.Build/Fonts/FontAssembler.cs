// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace CXEX.Build.Fonts;

/// <summary>
/// Turns a BMFont descriptor plus its atlas into an XFNT.
///
/// Slot assignment is the whole design decision here. ASCII 32..126 land on
/// slots 32..126 - identity, so every byte the console already prints keeps
/// printing the same glyph and nothing existing changes. Everything else is
/// assigned upward from 128 in codepoint order, and the slot-to-codepoint table
/// records the result. The font therefore DEFINES the OS's 8-bit codepage,
/// rather than any table in the kernel or the compiler claiming to.
///
/// The repertoire these fonts carry is DOS-flavoured but is not CP437: it has
/// no accented letters, no arrows and no pilcrow, and it adds rounded box
/// corners, dashed rules, diagonals and quarter moons that CP437 never had.
/// Mapping onto a fixed CP437 table would therefore have dropped glyphs and
/// invented others, which is why the mapping is data in the file.
/// </summary>
public static class FontAssembler
{
    public const int AsciiFirst = 32;
    public const int AsciiLast = 126;
    public const int ExtraFirstSlot = 128;

    public sealed record Rejection(uint Codepoint, int Width, int Height, string Why);

    /// <summary>
    /// Decide whether the glyphs live in the alpha channel or the colour
    /// channels, by looking INSIDE the glyph rectangles.
    ///
    /// Sampling the whole image is what got this wrong first time round: these
    /// atlases are transparent between the glyph boxes but fully opaque within
    /// them, so "the image has varying alpha" was true while "alpha describes
    /// the glyph" was false - and every pixel of every glyph came back as ink,
    /// producing 206 solid blocks. The only region that answers the question is
    /// the region the glyphs are actually read from.
    /// </summary>
    private static InkPolarity PickPolarity(BmFont font, PngImage atlas)
    {
        if (!PickChannel(font, atlas))
        {
            // Colour carries the shape. Which end of it is ink? A glyph is the
            // minority of its cell for text, so the rarer extreme is the ink -
            // measured over the glyph rects, which is the only region that can
            // answer it.
            long bright = 0, dark = 0;
            foreach (var g in font.Glyphs)
            {
                if (g.X < 0 || g.Y < 0 ||
                    g.X + g.Width > atlas.Width || g.Y + g.Height > atlas.Height) continue;
                for (int y = g.Y; y < g.Y + g.Height; y++)
                    for (int x = g.X; x < g.X + g.Width; x++)
                        if (atlas.Lum(x, y) >= 128) bright++; else dark++;
            }
            return bright <= dark ? InkPolarity.Light : InkPolarity.Dark;
        }
        return InkPolarity.Alpha;
    }

    private static bool PickChannel(BmFont font, PngImage atlas)
    {
        bool alphaVaries = false, lumVaries = false;
        foreach (var g in font.Glyphs)
        {
            if (g.X < 0 || g.Y < 0 ||
                g.X + g.Width > atlas.Width || g.Y + g.Height > atlas.Height) continue;
            for (int y = g.Y; y < g.Y + g.Height; y++)
                for (int x = g.X; x < g.X + g.Width; x++)
                {
                    if (atlas.Alpha(x, y) != 255) alphaVaries = true;
                    if (atlas.Lum(x, y) != atlas.Lum(g.X, g.Y)) lumVaries = true;
                }
            if (alphaVaries && lumVaries) break;
        }
        // Colour wins when it carries a shape: an alpha-keyed atlas has RGB
        // pinned at zero, so lumVaries is false there and alpha is used.
        return !lumVaries && alphaVaries;
    }

    public sealed record Result(
        byte[] Bytes,
        int CellWidth,
        int CellHeight,
        int Placed,
        List<Rejection> Rejected,
        List<(int Slot, uint Codepoint)> Assignments,
        bool UsedAlphaChannel,
        InkPolarity Polarity);

    /// <summary>
    /// `cellWidth`/`cellHeight` of 0 means "work it out": the width is the most
    /// common glyph width and the height is the descriptor's lineHeight. That
    /// gets 8x8 and 8x16 right for these fonts without being told.
    /// </summary>
    /// <summary>How to tell ink from background in the atlas.</summary>
    public enum InkPolarity
    {
        /// <summary>Measure the glyph rectangles and decide. The default.</summary>
        Auto,
        /// <summary>Bright pixels are ink (white glyph on black).</summary>
        Light,
        /// <summary>Dark pixels are ink (black glyph on white) - the §5.1 convention.</summary>
        Dark,
        /// <summary>Opaque pixels are ink, colour ignored (BMFont alphaChnl=1).</summary>
        Alpha,
    }

    public static Result Build(BmFont font, PngImage atlas, int cellWidth, int cellHeight,
                               int threshold = 128, bool strict = true,
                               InkPolarity polarity = InkPolarity.Auto)
    {
        if (cellWidth <= 0)
            cellWidth = font.Glyphs.GroupBy(g => g.Width)
                                   .OrderByDescending(g => g.Count()).ThenBy(g => g.Key)
                                   .First().Key;
        if (cellHeight <= 0)
            cellHeight = font.LineHeight > 0
                ? font.LineHeight
                : font.Glyphs.GroupBy(g => g.Height)
                             .OrderByDescending(g => g.Count()).ThenBy(g => g.Key)
                             .First().Key;

        if (cellWidth is < 1 or > XfntFormat.MaxWidth)
            throw new InvalidDataException($"cell width {cellWidth} is out of range");
        if (cellHeight is < 1 or > XfntFormat.MaxHeight)
            throw new InvalidDataException($"cell height {cellHeight} is out of range");

        // §5.1 of CX_DEVKIT_DESIGN.md states the glyph-sheet convention as
        // "white = background, black = glyph". These atlases are the other way
        // round - white glyphs on black - so the polarity is MEASURED by default
        // rather than assumed from either, and can be stated outright when a
        // sheet is ambiguous. Deciding it from the document would have inverted
        // every glyph in the repository's own fonts.
        InkPolarity effective = polarity == InkPolarity.Auto
            ? PickPolarity(font, atlas)
            : polarity;
        bool useAlpha = effective == InkPolarity.Alpha;
        bool darkInk = effective == InkPolarity.Dark;

        int bpr = XfntFormat.BytesPerRow(cellWidth);
        int bpg = bpr * cellHeight;

        var glyphs = new byte[]?[XfntFormat.MaxGlyphs];
        var codepoints = new uint[XfntFormat.MaxGlyphs];
        var rejected = new List<Rejection>();
        var assignments = new List<(int, uint)>();

        // Pass 1: ASCII keeps its own value as its slot.
        // Pass 2: everything else, in codepoint order, from slot 128 up.
        var ordered = font.Glyphs.OrderBy(g => g.Id).ToList();
        int nextExtra = ExtraFirstSlot;

        foreach (var g in ordered)
        {
            int slot;
            if (g.Id >= AsciiFirst && g.Id <= AsciiLast)
            {
                slot = (int)g.Id;
            }
            else
            {
                if (nextExtra >= XfntFormat.MaxGlyphs)
                {
                    rejected.Add(new Rejection(g.Id, g.Width, g.Height,
                        "no slot left: more than 128 non-ASCII glyphs"));
                    continue;
                }
                slot = nextExtra;
            }

            // The cell is the contract. A glyph wider or taller than the cell is
            // refused and named, which is what makes this a checker for a font
            // still being drawn rather than a converter that silently crops.
            if (g.Width > cellWidth || g.Height > cellHeight)
            {
                rejected.Add(new Rejection(g.Id, g.Width, g.Height,
                    $"{g.Width}x{g.Height} does not fit the {cellWidth}x{cellHeight} cell"));
                continue;
            }
            if (g.XOffset < 0 || g.YOffset < 0 ||
                g.XOffset + g.Width > cellWidth || g.YOffset + g.Height > cellHeight)
            {
                rejected.Add(new Rejection(g.Id, g.Width, g.Height,
                    $"offset {g.XOffset},{g.YOffset} pushes it outside the cell"));
                continue;
            }
            if (g.X < 0 || g.Y < 0 ||
                g.X + g.Width > atlas.Width || g.Y + g.Height > atlas.Height)
            {
                rejected.Add(new Rejection(g.Id, g.Width, g.Height,
                    $"source rect {g.X},{g.Y} {g.Width}x{g.Height} is outside the {atlas.Width}x{atlas.Height} atlas"));
                continue;
            }

            var bits = new byte[bpg];
            for (int row = 0; row < g.Height; row++)
            {
                for (int col = 0; col < g.Width; col++)
                {
                    if (!atlas.Ink(g.X + col, g.Y + row, useAlpha, threshold, darkInk)) continue;
                    int px = g.XOffset + col;
                    int py = g.YOffset + row;
                    // MSB first: 0x80 is the leftmost pixel of the byte, which is
                    // the order fb_draw_char already reads with (0x80 >> col).
                    bits[py * bpr + (px >> 3)] |= (byte)(0x80 >> (px & 7));
                }
            }

            glyphs[slot] = bits;
            codepoints[slot] = g.Id;
            assignments.Add((slot, g.Id));
            if (slot >= ExtraFirstSlot) nextExtra++;
        }

        if (strict && rejected.Count > 0)
            throw new FontNotReadyException(rejected, cellWidth, cellHeight);

        var bytes = XfntFormat.Write(cellWidth, cellHeight, glyphs, codepoints, font.Face);
        return new Result(bytes, cellWidth, cellHeight, assignments.Count, rejected,
                          assignments, useAlpha, effective);
    }
}

/// <summary>
/// Raised when glyphs do not fit the cell. Carries the list, because the list is
/// the useful part: it is exactly the set still to be adjusted.
/// </summary>
public sealed class FontNotReadyException : Exception
{
    public IReadOnlyList<FontAssembler.Rejection> Rejected { get; }
    public int CellWidth { get; }
    public int CellHeight { get; }

    public FontNotReadyException(IReadOnlyList<FontAssembler.Rejection> rejected, int w, int h)
        : base($"{rejected.Count} glyph(s) do not fit the {w}x{h} cell")
    {
        Rejected = rejected; CellWidth = w; CellHeight = h;
    }
}
