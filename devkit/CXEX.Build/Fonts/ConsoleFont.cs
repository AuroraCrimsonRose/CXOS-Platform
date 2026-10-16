// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Linq;

namespace CXEX.Build.Fonts;

/// <summary>
/// A console font: a published font, cut down to one standard character set
/// and laid out as the kernel's console expects.
///
/// <para>The console draws one byte as one glyph, by slot. Each slot is filled
/// with the character that byte means in a standard 8-bit character set, so a
/// byte means the same thing in CXOS as in that set everywhere else, and the
/// font's codepoint table records it. No slot holds a character chosen here.</para>
///
/// <para>Strict: every character of the set must be in the font. A font that
/// lacks one fails the build with the missing codepoints named, instead of
/// shipping a console with blank cells where the set promises characters.</para>
/// </summary>
public static class ConsoleFont
{
    /// <summary>The character sets a console font can be built for.</summary>
    public enum Charset
    {
        /// <summary>IBM code page 437, the PC character set: ASCII, then
        /// Western European letters, box drawing, shading and symbols.</summary>
        Cp437,
    }

    public sealed record Result(byte[] Bytes, int CellWidth, int CellHeight, int Placed, string Face);

    /// <summary>The slots the set defines. Slots 0..31 and 127 are control
    /// characters in every set and stay blank: the console acts on them rather
    /// than drawing them.</summary>
    public static IEnumerable<(int Slot, uint Codepoint)> Slots(Charset set) => set switch
    {
        Charset.Cp437 => Enumerable.Range(0x20, 0x5F).Select(s => (s, (uint)s))
                        .Concat(Enumerable.Range(0, 128).Select(i => (0x80 + i, (uint)CodePage437.Upper[i]))),
        _ => throw new ArgumentOutOfRangeException(nameof(set)),
    };

    /// <param name="face">The name stored in the font. Never defaulted from the source: a
    /// converted or subset OFL font is a Modified Version, which may not carry
    /// the source's Reserved Font Name (Terminus reserves "Terminus Font").</param>
    public static Result Build(BdfFont font, Charset set, string face)
    {
        if (font.CellWidth > XfntFormat.MaxWidth || font.CellHeight > XfntFormat.MaxHeight)
            throw new InvalidOperationException(
                $"the font's cell is {font.CellWidth}x{font.CellHeight}; XFNT holds at most " +
                $"{XfntFormat.MaxWidth}x{XfntFormat.MaxHeight}");

        var slots = Slots(set).ToList();
        var missing = slots.Where(s => !font.Glyphs.ContainsKey(s.Codepoint))
                           .Select(s => s.Codepoint).ToList();
        if (missing.Count > 0)
            throw new FontNotReadyException(missing, set);

        var glyphs = new byte[]?[XfntFormat.MaxGlyphs];
        var codepoints = new uint[XfntFormat.MaxGlyphs];
        foreach (var (slot, cp) in slots)
        {
            glyphs[slot] = font.Glyphs[cp].SelectMany(r => r).ToArray();
            codepoints[slot] = cp;
        }

        if (string.IsNullOrWhiteSpace(face)) throw new ArgumentException("a face name is required", nameof(face));
        string name = face;
        byte[] bytes = XfntFormat.Write(font.CellWidth, font.CellHeight, glyphs, codepoints, name);
        return new Result(bytes, font.CellWidth, font.CellHeight, slots.Count, name.Length > 8 ? name[..8] : name);
    }
}

/// <summary>The font lacks characters the requested set needs.</summary>
public sealed class FontNotReadyException : Exception
{
    public IReadOnlyList<uint> Missing { get; }
    public ConsoleFont.Charset Set { get; }

    public FontNotReadyException(IReadOnlyList<uint> missing, ConsoleFont.Charset set)
        : base($"the font lacks {missing.Count} character(s) of {set}: " +
               string.Join(" ", missing.Select(c => $"U+{c:X4}")))
    {
        Missing = missing;
        Set = set;
    }
}
