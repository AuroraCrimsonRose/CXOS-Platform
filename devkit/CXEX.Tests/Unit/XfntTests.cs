// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using CXEX.Build.Fonts;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// XFNT: the format the kernel reads, and the converter that produces it.
///
/// The converter matters more than it looks, because its failure mode is silent.
/// A wrong ink test does not throw and does not produce a short file - it
/// produces a font of the right size, with the right glyph count, in which
/// every glyph is a solid block. Nothing but looking at the pixels catches
/// that, so the pixels are what these assert.
/// </summary>
public class XfntTests
{
    // ---- a tiny atlas, built in memory ------------------------------------
    //
    // Two 8x8 glyphs side by side: a vertical bar on the left half of the
    // first, and a single top-left dot in the second. Distinct enough that a
    // transposed or solid-filled read is obvious.

    private static byte[] MakePng(int w, int h, Func<int, int, (byte r, byte g, byte b, byte a)> px)
    {
        var raw = new byte[h * (1 + w * 4)];
        for (int y = 0; y < h; y++)
        {
            int at = y * (1 + w * 4);
            raw[at] = 0;                       // filter: none
            for (int x = 0; x < w; x++)
            {
                var (r, g, b, a) = px(x, y);
                raw[at + 1 + x * 4 + 0] = r;
                raw[at + 1 + x * 4 + 1] = g;
                raw[at + 1 + x * 4 + 2] = b;
                raw[at + 1 + x * 4 + 3] = a;
            }
        }

        using var idat = new MemoryStream();
        using (var z = new ZLibStream(idat, CompressionLevel.Optimal, leaveOpen: true))
            z.Write(raw, 0, raw.Length);
        byte[] compressed = idat.ToArray();

        using var ms = new MemoryStream();
        ms.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
        WriteChunk(ms, "IHDR", Ihdr(w, h));
        WriteChunk(ms, "IDAT", compressed);
        WriteChunk(ms, "IEND", Array.Empty<byte>());
        return ms.ToArray();

        static byte[] Ihdr(int w, int h)
        {
            var d = new byte[13];
            WriteBe32(d, 0, w); WriteBe32(d, 4, h);
            d[8] = 8;    // bit depth
            d[9] = 6;    // RGBA
            return d;
        }
    }

    private static void WriteChunk(Stream s, string type, byte[] data)
    {
        var len = new byte[4]; WriteBe32(len, 0, data.Length); s.Write(len);
        var t = System.Text.Encoding.ASCII.GetBytes(type);
        s.Write(t); s.Write(data);
        uint crc = Crc32(t.Concat(data).ToArray());
        var c = new byte[4]; WriteBe32(c, 0, (int)crc); s.Write(c);
    }

    private static void WriteBe32(byte[] b, int at, int v)
    {
        b[at] = (byte)(v >> 24); b[at + 1] = (byte)(v >> 16);
        b[at + 2] = (byte)(v >> 8); b[at + 3] = (byte)v;
    }

    private static uint Crc32(byte[] data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int i = 0; i < 8; i++)
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
        }
        return ~crc;
    }

    private const string TwoGlyphFnt = """
        info face="T" size=8 bold=0 italic=0 unicode=1
        common lineHeight=8 base=8 scaleW=16 scaleH=8 pages=1
        page id=0 file="atlas.png"
        chars count=2
        char id=65 x=0 y=0 width=8 height=8 xoffset=0 yoffset=0 xadvance=8 page=0 chnl=15
        char id=9472 x=8 y=0 width=8 height=8 xoffset=0 yoffset=0 xadvance=8 page=0 chnl=15
        """;

    /// <summary>Ink in the COLOUR channels, glyph boxes fully opaque.</summary>
    private static PngImage ColourAtlas() => PngImage.Parse(MakePng(16, 8, (x, y) =>
    {
        bool on = x < 8 ? x is 1 or 2 : (x == 8 && y == 0);
        return on ? ((byte)255, (byte)255, (byte)255, (byte)255)
                  : ((byte)0, (byte)0, (byte)0, (byte)255);
    }));

    /// <summary>The same shapes, keyed into ALPHA with RGB at zero.</summary>
    private static PngImage AlphaAtlas() => PngImage.Parse(MakePng(16, 8, (x, y) =>
    {
        bool on = x < 8 ? x is 1 or 2 : (x == 8 && y == 0);
        return ((byte)0, (byte)0, (byte)0, on ? (byte)255 : (byte)0);
    }));

    private static BmFont Font() => BmFont.Parse(TwoGlyphFnt);

    // ---- the control, and the regression it guards -------------------------

    [Fact]
    public void A_colour_keyed_atlas_yields_the_drawn_shape()
    {
        var r = FontAssembler.Build(Font(), ColourAtlas(), 0, 0);
        var h = XfntFormat.Read(r.Bytes);

        // columns 1 and 2 set on every row: 0b01100000
        var a = XfntFormat.Glyph(r.Bytes, h, 65);
        Assert.All(a, row => Assert.Equal(0x60, row));

        // one dot, top-left only
        var bar = XfntFormat.Glyph(r.Bytes, h, 128);
        Assert.Equal(0x80, bar[0]);
        Assert.All(bar.Skip(1), row => Assert.Equal(0, row));
    }

    [Fact]
    public void An_alpha_keyed_atlas_yields_the_same_shape()
    {
        var r = FontAssembler.Build(Font(), AlphaAtlas(), 0, 0);
        var h = XfntFormat.Read(r.Bytes);
        Assert.All(XfntFormat.Glyph(r.Bytes, h, 65), row => Assert.Equal(0x60, row));
    }

    /// <summary>
    /// The regression that produced 206 solid blocks. These atlases are
    /// transparent BETWEEN the glyph boxes and opaque within them, so "the
    /// image has varying alpha" is true while "alpha describes the glyph" is
    /// false. Choosing the channel from the whole image instead of from the
    /// glyph rectangles marks every pixel as ink.
    /// </summary>
    [Fact]
    public void Transparent_padding_around_opaque_glyphs_does_not_select_the_alpha_channel()
    {
        // identical to ColourAtlas, except the single pixel column at x=15 -
        // outside both glyph rects - is transparent.
        var atlas = PngImage.Parse(MakePng(16, 8, (x, y) =>
        {
            if (x == 15) return ((byte)0, (byte)0, (byte)0, (byte)0);
            bool on = x < 8 ? x is 1 or 2 : (x == 8 && y == 0);
            return on ? ((byte)255, (byte)255, (byte)255, (byte)255)
                      : ((byte)0, (byte)0, (byte)0, (byte)255);
        }));

        var r = FontAssembler.Build(Font(), atlas, 0, 0);
        var h = XfntFormat.Read(r.Bytes);
        var a = XfntFormat.Glyph(r.Bytes, h, 65);

        Assert.False(r.UsedAlphaChannel);
        Assert.All(a, row => Assert.NotEqual(0xFF, row));   // not a solid block
        Assert.All(a, row => Assert.Equal(0x60, row));
    }

    // ---- slot assignment ---------------------------------------------------

    [Fact]
    public void Ascii_keeps_its_own_value_as_its_slot_and_extras_start_at_128()
    {
        var r = FontAssembler.Build(Font(), ColourAtlas(), 0, 0);
        Assert.Contains((65, 65u), r.Assignments);
        Assert.Contains((128, 9472u), r.Assignments);

        var h = XfntFormat.Read(r.Bytes);
        Assert.Equal(65u, XfntFormat.Codepoint(r.Bytes, h, 65));
        Assert.Equal(9472u, XfntFormat.Codepoint(r.Bytes, h, 128));
        Assert.Equal(0u, XfntFormat.Codepoint(r.Bytes, h, 66));   // nothing there
    }

    [Fact]
    public void The_cell_is_derived_from_the_glyphs_and_the_line_height()
    {
        var r = FontAssembler.Build(Font(), ColourAtlas(), 0, 0);
        Assert.Equal(8, r.CellWidth);
        Assert.Equal(8, r.CellHeight);
    }

    // ---- the checker -------------------------------------------------------

    [Fact]
    public void A_glyph_bigger_than_the_cell_is_refused_and_named()
    {
        // The real case: term_16's unadjusted glyphs are 16x16 in an 8x16 cell.
        const string fnt = """
            info face="T" size=16 unicode=1
            common lineHeight=16 base=16 scaleW=32 scaleH=16 pages=1
            page id=0 file="atlas.png"
            chars count=2
            char id=65 x=0 y=0 width=8 height=16 xoffset=0 yoffset=0 xadvance=16 page=0 chnl=15
            char id=9632 x=8 y=0 width=16 height=16 xoffset=0 yoffset=0 xadvance=16 page=0 chnl=15
            """;
        var atlas = PngImage.Parse(MakePng(32, 16, (x, y) => ((byte)255, (byte)255, (byte)255, (byte)255)));

        var ex = Assert.Throws<FontNotReadyException>(
            () => FontAssembler.Build(BmFont.Parse(fnt), atlas, 0, 0));

        Assert.Equal(8, ex.CellWidth);
        Assert.Equal(16, ex.CellHeight);
        var bad = Assert.Single(ex.Rejected);
        Assert.Equal(9632u, bad.Codepoint);

        // ...and --allow-unfit writes the font with that slot blank rather than
        // cropping a 16-wide glyph into an 8-wide cell.
        var r = FontAssembler.Build(BmFont.Parse(fnt), atlas, 0, 0, strict: false);
        var h = XfntFormat.Read(r.Bytes);
        Assert.Single(r.Rejected);
        Assert.All(XfntFormat.Glyph(r.Bytes, h, 128), b => Assert.Equal(0, b));
    }

    [Fact]
    public void A_source_rect_outside_the_atlas_is_refused_rather_than_read()
    {
        const string fnt = """
            info face="T" size=8 unicode=1
            common lineHeight=8 base=8 pages=1
            page id=0 file="atlas.png"
            chars count=1
            char id=65 x=250 y=0 width=8 height=8 xoffset=0 yoffset=0 xadvance=8 page=0 chnl=15
            """;
        var atlas = PngImage.Parse(MakePng(16, 8, (x, y) => ((byte)255, (byte)255, (byte)255, (byte)255)));
        var ex = Assert.Throws<FontNotReadyException>(
            () => FontAssembler.Build(BmFont.Parse(fnt), atlas, 0, 0));
        Assert.Contains("outside", Assert.Single(ex.Rejected).Why);
    }

    // ---- the format itself -------------------------------------------------

    [Fact]
    public void A_written_font_reads_back_with_the_geometry_it_was_given()
    {
        var glyphs = new byte[]?[256];
        for (int i = 0; i < 256; i++) glyphs[i] = Enumerable.Repeat((byte)i, 16).ToArray();
        var bytes = XfntFormat.Write(8, 16, glyphs, null, "Term16");

        var h = XfntFormat.Read(bytes);
        Assert.Equal(8, h.Width);
        Assert.Equal(16, h.Height);
        Assert.Equal(256, h.GlyphCount);
        Assert.Equal(1, h.BytesPerRowValue);
        Assert.Equal("Term16", h.Face);
        Assert.False(h.HasCodepoints);
        Assert.Equal(XfntFormat.HeaderSize + 256 * 16, bytes.Length);
        Assert.All(XfntFormat.Glyph(bytes, h, 7), b => Assert.Equal(7, b));
    }

    [Theory]
    [InlineData(0, "bad magic")]        // magic
    [InlineData(4, "version")]          // version
    [InlineData(6, "kind")]             // bitmap vs vector
    [InlineData(7, "bytes_per_row")]    // bytes_per_row vs width
    [InlineData(8, "width")]            // width
    [InlineData(9, "height")]           // height
    public void A_header_field_that_disagrees_with_the_file_is_refused(int offset, string expect)
    {
        var glyphs = new byte[]?[4];
        for (int i = 0; i < 4; i++) glyphs[i] = new byte[16];
        var bytes = XfntFormat.Write(8, 16, glyphs, null, "T");

        bytes[offset] ^= 0xFF;
        var ex = Assert.Throws<InvalidDataException>(() => XfntFormat.Read(bytes));
        Assert.Contains(expect, ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// The length check is the one that matters most, because it is what stops a
    /// reader indexing past the end: the header must describe the file EXACTLY,
    /// and the comparison is against the real length rather than anything the
    /// file says about itself.
    /// </summary>
    [Fact]
    public void A_truncated_or_padded_file_is_refused()
    {
        var glyphs = new byte[]?[4];
        for (int i = 0; i < 4; i++) glyphs[i] = new byte[16];
        var bytes = XfntFormat.Write(8, 16, glyphs, null, "T");

        Assert.Contains("describes", Assert.Throws<InvalidDataException>(
            () => XfntFormat.Read(bytes[..^1])).Message);
        Assert.Contains("describes", Assert.Throws<InvalidDataException>(
            () => XfntFormat.Read(bytes.Concat(new byte[] { 0 }).ToArray())).Message);
    }

    [Fact]
    public void A_glyph_count_claiming_more_than_the_file_holds_is_refused()
    {
        var glyphs = new byte[]?[4];
        for (int i = 0; i < 4; i++) glyphs[i] = new byte[16];
        var bytes = XfntFormat.Write(8, 16, glyphs, null, "T");

        bytes[10] = 200;   // glyph_count low byte: 4 -> 200
        Assert.Throws<InvalidDataException>(() => XfntFormat.Read(bytes));
    }

    /// <summary>
    /// A vector XFNT is refused by name. CX_DEVKIT_DESIGN.md §5.1 locks XFNT as
    /// the container for vector fonts too, so one is a legitimate file this
    /// reader cannot draw - and saying so beats reading its glyph data as
    /// though it were rows of pixels.
    /// </summary>
    [Fact]
    public void A_vector_font_is_refused_as_unimplemented_rather_than_misread()
    {
        var glyphs = new byte[]?[4];
        for (int i = 0; i < 4; i++) glyphs[i] = new byte[16];
        var bytes = XfntFormat.Write(8, 16, glyphs, null, "T");

        Assert.Equal(XfntFormat.KindBitmap, bytes[6]);
        bytes[6] = XfntFormat.KindVector;
        Assert.Contains("vector", Assert.Throws<InvalidDataException>(
            () => XfntFormat.Read(bytes)).Message);
    }

    // ---- PNG ---------------------------------------------------------------

    [Fact]
    public void Every_scanline_filter_reconstructs_to_the_same_image()
    {
        // Paeth and friends are the part of PNG most easily got subtly wrong,
        // and a wrong filter still inflates to the right LENGTH.
        foreach (byte filter in new byte[] { 0, 1, 2, 3, 4 })
        {
            var png = MakeFilteredPng(8, 4, filter);
            var img = PngImage.Parse(png);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++)
                    Assert.Equal((byte)(x * 16 + y), img.Lum(x, y));
        }
    }

    [Fact]
    public void An_interlaced_or_wrong_depth_png_is_refused_by_name()
    {
        var png = MakePng(8, 8, (x, y) => ((byte)0, (byte)0, (byte)0, (byte)255));
        png[8 + 8 + 12] = 1;                   // IHDR interlace = 1
        Assert.Contains("interlaced", Assert.Throws<InvalidDataException>(
            () => PngImage.Parse(png)).Message);

        var png2 = MakePng(8, 8, (x, y) => ((byte)0, (byte)0, (byte)0, (byte)255));
        png2[8 + 8 + 8] = 16;                  // bit depth = 16
        Assert.Contains("8-bit", Assert.Throws<InvalidDataException>(
            () => PngImage.Parse(png2)).Message);
    }

    /// <summary>Same pixels every time, encoded with one chosen filter.</summary>
    private static byte[] MakeFilteredPng(int w, int h, byte filter)
    {
        byte Want(int x, int y) => (byte)(x * 16 + y);

        var raw = new byte[h * (1 + w * 4)];
        var prev = new byte[w * 4];
        for (int y = 0; y < h; y++)
        {
            var line = new byte[w * 4];
            for (int x = 0; x < w; x++)
            {
                line[x * 4 + 0] = Want(x, y);
                line[x * 4 + 1] = Want(x, y);
                line[x * 4 + 2] = Want(x, y);
                line[x * 4 + 3] = 255;
            }

            var enc = new byte[w * 4];
            for (int i = 0; i < line.Length; i++)
            {
                int a = i >= 4 ? line[i - 4] : 0, b = prev[i], c = i >= 4 ? prev[i - 4] : 0;
                enc[i] = filter switch
                {
                    0 => line[i],
                    1 => (byte)(line[i] - a),
                    2 => (byte)(line[i] - b),
                    3 => (byte)(line[i] - ((a + b) >> 1)),
                    _ => (byte)(line[i] - Paeth(a, b, c)),
                };
            }

            int at = y * (1 + w * 4);
            raw[at] = filter;
            Buffer.BlockCopy(enc, 0, raw, at + 1, enc.Length);
            prev = line;
        }

        using var idat = new MemoryStream();
        using (var z = new ZLibStream(idat, CompressionLevel.Optimal, leaveOpen: true))
            z.Write(raw, 0, raw.Length);

        using var ms = new MemoryStream();
        ms.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
        var ihdr = new byte[13];
        WriteBe32(ihdr, 0, w); WriteBe32(ihdr, 4, h); ihdr[8] = 8; ihdr[9] = 6;
        WriteChunk(ms, "IHDR", ihdr);
        WriteChunk(ms, "IDAT", idat.ToArray());
        WriteChunk(ms, "IEND", Array.Empty<byte>());
        return ms.ToArray();

        static int Paeth(int a, int b, int c)
        {
            int p = a + b - c;
            int pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
            return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        }
    }

    // ---- the real fonts ----------------------------------------------------

    /// <summary>
    /// The repository's own fonts, when they are present. term_8 is complete, so
    /// it must convert; term_16 is still being drawn, so it is only required to
    /// convert with --allow-unfit. Skipped rather than failed when the assets
    /// are not beside the test run.
    /// </summary>
    [Fact]
    public void The_repositorys_8x8_font_converts_and_its_capital_A_has_ink()
    {
        string? dir = FindAssets();
        if (dir is null) return;

        var font = BmFont.Load(Path.Combine(dir, "term_8.fnt"));
        var atlas = PngImage.Load(Path.Combine(dir, font.PageFile));
        var r = FontAssembler.Build(font, atlas, 0, 0);

        Assert.Equal(8, r.CellWidth);
        Assert.Equal(8, r.CellHeight);
        Assert.Equal(206, r.Placed);
        Assert.Empty(r.Rejected);

        var h = XfntFormat.Read(r.Bytes);
        var a = XfntFormat.Glyph(r.Bytes, h, 65);
        Assert.Contains(a, b => b != 0);                 // 'A' is not blank
        Assert.DoesNotContain(a, b => b == 0xFF);        // nor a solid block
        Assert.Equal(65u, XfntFormat.Codepoint(r.Bytes, h, 65));
    }

    private static string? FindAssets()
    {
        var d = new DirectoryInfo(AppContext.BaseDirectory);
        while (d is not null)
        {
            string p = Path.Combine(d.FullName, "assets", "fonts", "cx_term");
            if (File.Exists(Path.Combine(p, "term_8.fnt"))) return p;
            d = d.Parent;
        }
        return null;
    }
}
