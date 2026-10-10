// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;

namespace CXEX.Build.Fonts;

/// <summary>
/// XFNT - X Format foNT. A bitmap font as literally the bits, plus a header a
/// reader can validate with comparisons instead of parsing.
///
/// Why this exists rather than loading a BMFont .fnt in the kernel: a .fnt
/// carries no pixels at all. The glyphs live in a companion PNG, so "load the
/// .fnt" would mean a PNG decoder - zlib inflate, chunk walking, CRCs, filter
/// reconstruction - and an ASCII key=value parser, both in ring 0, both over
/// untrusted input. The conversion happens here on the host instead, and the
/// kernel reads a fixed layout. Same split as CXEX: the DevKit produces, the
/// kernel validates.
///
/// Glyphs are indexed by SLOT, not by codepoint. A slot is a byte, so the
/// kernel's lookup is base + slot * bytes_per_glyph with no search, and the
/// console keeps its one-byte char. The optional codepoint table records what
/// each slot MEANS, which keeps the mapping in the file rather than hardcoded
/// in a table somewhere - and lets a future Unicode-aware console use the same
/// font without a format change.
/// </summary>
public static class XfntFormat
{
    public const uint Magic = 0x544E4658;      // 'X','F','N','T' little-endian
    public const ushort Version = 1;
    public const int HeaderSize = 24;

    /// <summary>
    /// Bitmap or vector. CX_DEVKIT_DESIGN.md §5.1 locks XFNT as the container
    /// for BOTH - "a header describing whether contents are scalable vector or
    /// bitmap" - so the field exists from v1 even though only bitmap is
    /// implemented. Leaving it out would have meant a version bump the first
    /// time a vector font appeared, for a byte that costs nothing now.
    /// </summary>
    public const byte KindBitmap = 1;
    public const byte KindVector = 2;   // reserved; nothing writes or reads it yet

    /// <summary>Row bytes are most-significant-bit-first: 0x80 is the leftmost pixel.</summary>
    public const ushort FlagMsbFirst = 0x0001;
    /// <summary>A slot-to-codepoint table follows the glyph data.</summary>
    public const ushort FlagHasCodepoints = 0x0002;

    // The format states its own ceilings rather than leaving each reader to
    // invent one - and they are deliberately the SAME ceilings the kernel
    // enforces, so a font the converter writes is one the kernel can read. A
    // host that can emit something the kernel refuses moves the failure to
    // boot, as far as possible from the command that caused it.
    //
    // Width is capped at 8 for v1: one byte per row keeps every offset a shift,
    // and a console cell wider than a byte complicates the blit on both sides
    // for no font anyone here is drawing. bytes_per_row is still a header field,
    // so widening it later is a version bump and not a layout change.
    public const int MaxWidth = 8;
    public const int MaxHeight = 32;
    public const int MaxGlyphs = 256;

    public static int BytesPerRow(int width) => (width + 7) / 8;
    public static int BytesPerGlyph(int width, int height) => BytesPerRow(width) * height;

    /// <summary>
    /// Serialize a font. `glyphs[slot]` is BytesPerGlyph bytes, row-major, MSB
    /// first; a null entry is a blank slot. `codepoints[slot]` is 0 where the
    /// slot means nothing.
    /// </summary>
    public static byte[] Write(int width, int height, IReadOnlyList<byte[]?> glyphs,
                               IReadOnlyList<uint>? codepoints, string face)
    {
        if (width is < 1 or > MaxWidth)   throw new ArgumentOutOfRangeException(nameof(width));
        if (height is < 1 or > MaxHeight) throw new ArgumentOutOfRangeException(nameof(height));
        if (glyphs.Count is < 1 or > MaxGlyphs) throw new ArgumentOutOfRangeException(nameof(glyphs));
        if (codepoints is not null && codepoints.Count != glyphs.Count)
            throw new ArgumentException("codepoint table must have one entry per glyph slot", nameof(codepoints));

        int bpr = BytesPerRow(width);
        int bpg = bpr * height;

        ushort flags = FlagMsbFirst;
        if (codepoints is not null) flags |= FlagHasCodepoints;

        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);

        w.Write(Magic);                      // 0
        w.Write(Version);                    // 4
        w.Write(KindBitmap);                 // 6
        w.Write((byte)bpr);                  // 7
        w.Write((byte)width);                // 8
        w.Write((byte)height);               // 9
        w.Write((ushort)glyphs.Count);       // 10
        w.Write(flags);                      // 12
        w.Write((ushort)0);                  // 14 reserved
        // A short face name, so a font can be identified without a side file.
        // Fixed 8 bytes, NUL padded, never NUL terminated by contract - readers
        // treat it as a fixed field, which removes the unterminated-string
        // question entirely rather than answering it.
        var nameBytes = new byte[8];
        for (int i = 0; i < 8 && i < face.Length; i++)
            nameBytes[i] = (byte)(face[i] is >= ' ' and < (char)127 ? face[i] : '?');
        w.Write(nameBytes);                  // 16..23

        foreach (var g in glyphs)
        {
            if (g is null) { w.Write(new byte[bpg]); continue; }
            if (g.Length != bpg)
                throw new ArgumentException($"glyph is {g.Length} bytes, expected {bpg}");
            w.Write(g);
        }

        if (codepoints is not null)
            foreach (uint cp in codepoints) w.Write(cp);

        return ms.ToArray();
    }

    public sealed record Header(int Width, int Height, int GlyphCount, int BytesPerRowValue,
                                ushort Flags, string Face)
    {
        public int BytesPerGlyphValue => BytesPerRowValue * Height;
        public bool HasCodepoints => (Flags & FlagHasCodepoints) != 0;
    }

    /// <summary>
    /// Validate and parse a header. Deliberately the same checks the kernel
    /// makes, in the same order, so a font the host accepts is one the kernel
    /// accepts - the independent-validation point cuts both ways, and a
    /// converter that emits something the kernel refuses is a worse bug than
    /// one that refuses to emit.
    /// </summary>
    public static Header Read(byte[] file)
    {
        if (file.Length < HeaderSize) throw new InvalidDataException("shorter than the header");
        if (BitConverter.ToUInt32(file, 0) != Magic) throw new InvalidDataException("not an XFNT (bad magic)");
        ushort ver = BitConverter.ToUInt16(file, 4);
        if (ver != Version) throw new InvalidDataException($"unsupported XFNT version {ver}");

        byte kind = file[6];
        if (kind != KindBitmap)
            throw new InvalidDataException(kind == KindVector
                ? "this is a vector XFNT; only bitmap fonts are implemented"
                : $"unknown XFNT kind {kind}");

        int bpr = file[7];
        int width = file[8], height = file[9];
        int count = BitConverter.ToUInt16(file, 10);
        ushort flags = BitConverter.ToUInt16(file, 12);

        if (width is < 1 or > MaxWidth)   throw new InvalidDataException($"glyph width {width} out of range");
        if (height is < 1 or > MaxHeight) throw new InvalidDataException($"glyph height {height} out of range");
        if (count is < 1 or > MaxGlyphs)  throw new InvalidDataException($"glyph count {count} out of range");
        if (bpr != BytesPerRow(width))
            throw new InvalidDataException($"bytes_per_row {bpr} disagrees with width {width}");
        if ((flags & FlagMsbFirst) == 0)
            throw new InvalidDataException("LSB-first rows are not supported");

        // The length must be EXACTLY what the header describes. Computed in 64
        // bits and compared against the real file length, never against a size
        // the file declares about itself.
        long need = (long)HeaderSize + (long)count * bpr * height;
        if ((flags & FlagHasCodepoints) != 0) need += (long)count * 4;
        if (file.Length != need)
            throw new InvalidDataException($"file is {file.Length} bytes, header describes {need}");

        var face = System.Text.Encoding.ASCII.GetString(file, 16, 8).TrimEnd('\0');
        return new Header(width, height, count, bpr, flags, face);
    }

    public static byte[] Glyph(byte[] file, Header h, int slot)
    {
        if ((uint)slot >= (uint)h.GlyphCount) throw new ArgumentOutOfRangeException(nameof(slot));
        var g = new byte[h.BytesPerGlyphValue];
        Buffer.BlockCopy(file, HeaderSize + slot * h.BytesPerGlyphValue, g, 0, g.Length);
        return g;
    }

    public static uint Codepoint(byte[] file, Header h, int slot)
    {
        if (!h.HasCodepoints) return 0;
        if ((uint)slot >= (uint)h.GlyphCount) throw new ArgumentOutOfRangeException(nameof(slot));
        int off = HeaderSize + h.GlyphCount * h.BytesPerGlyphValue + slot * 4;
        return BitConverter.ToUInt32(file, off);
    }
}
