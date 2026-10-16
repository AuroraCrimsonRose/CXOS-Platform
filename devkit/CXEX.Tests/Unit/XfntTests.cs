// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using CXEX.Build.Fonts;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// XFNT: the format the kernel reads, and the converter that produces it from
/// a published BDF font.
///
/// The converter's failure mode is silent. A misplaced glyph or a wrong code
/// page does not throw and does not produce a short file - it produces a font
/// of the right size in which a character is in the wrong slot or shifted out
/// of its cell. So the pixels and the slots are what these assert.
/// </summary>
public class XfntTests
{
    // ---- a tiny BDF, built in memory ----------------------------------------
    //
    // A 4x6 cell with the baseline one row above the bottom. 'A' is a full-box
    // glyph; '.' is a single pixel on the baseline at column 1, given its own
    // small BBX so placement relative to the cell is actually exercised.
    private static string Bdf(string chars, int count) =>
        "STARTFONT 2.1\n" +
        "FONT -Test-Tiny-Medium-R-Normal--6-60-72-72-C-40-ISO10646-1\n" +
        "SIZE 6 72 72\n" +
        "FONTBOUNDINGBOX 4 6 0 -1\n" +
        "STARTPROPERTIES 1\n" +
        "FAMILY_NAME \"Tiny\"\n" +
        "ENDPROPERTIES\n" +
        $"CHARS {count}\n" +
        chars + "\n" +
        "ENDFONT\n";

    private const string GlyphA =
        "STARTCHAR A\nENCODING 65\nSWIDTH 500 0\nDWIDTH 4 0\nBBX 4 6 0 -1\nBITMAP\n" +
        "60\n90\nF0\n90\n90\n00\nENDCHAR";

    private const string GlyphDot =
        "STARTCHAR period\nENCODING 46\nSWIDTH 500 0\nDWIDTH 4 0\nBBX 1 1 1 0\nBITMAP\n" +
        "80\nENDCHAR";

    [Fact]
    public void A_glyph_is_placed_in_the_cell_by_its_box()
    {
        var font = BdfFont.Parse(Bdf(GlyphA + "\n" + GlyphDot, 2));

        Assert.Equal(4, font.CellWidth);
        Assert.Equal(6, font.CellHeight);
        Assert.Equal("Tiny", font.Family);

        // 'A' fills the box: rows as written, MSB first.
        var a = font.Glyphs['A'];
        Assert.Equal(new byte[] { 0x60, 0x90, 0xF0, 0x90, 0x90, 0x00 }, a.Select(r => r[0]).ToArray());

        // '.' is one pixel: BBX 1x1 at (1,0) is column 1 on the baseline, and
        // the baseline is row 4 of a 6-row cell whose bottom is one row below it.
        var dot = font.Glyphs['.'];
        for (int r = 0; r < 6; r++)
            Assert.Equal(r == 4 ? (byte)0x40 : (byte)0x00, dot[r][0]);
    }

    [Fact]
    public void A_glyph_outside_the_cell_is_refused_and_named()
    {
        string wide = GlyphDot.Replace("BBX 1 1 1 0", "BBX 1 1 4 0");   // column 4 of a 4-wide cell
        var ex = Assert.Throws<InvalidDataException>(() => BdfFont.Parse(Bdf(wide, 1)));
        Assert.Contains("U+002E", ex.Message);
        Assert.Contains("does not fit", ex.Message);
    }

    [Fact]
    public void A_bitmap_with_the_wrong_number_of_rows_is_refused()
    {
        string shortA = GlyphA.Replace("90\n00\nENDCHAR", "90\nENDCHAR");
        Assert.Contains("rows", Assert.Throws<InvalidDataException>(() => BdfFont.Parse(Bdf(shortA, 1))).Message);

        string longA = GlyphA.Replace("00\nENDCHAR", "00\n00\nENDCHAR");
        Assert.Contains("more rows", Assert.Throws<InvalidDataException>(() => BdfFont.Parse(Bdf(longA, 1))).Message);
    }

    [Fact]
    public void A_codepoint_defined_twice_is_refused()
    {
        var ex = Assert.Throws<InvalidDataException>(() => BdfFont.Parse(Bdf(GlyphA + "\n" + GlyphA, 2)));
        Assert.Contains("twice", ex.Message);
    }

    [Fact]
    public void Text_that_is_not_bdf_is_refused()
    {
        Assert.Throws<InvalidDataException>(() => BdfFont.Parse("info face=\"T\" size=8"));
    }

    // ---- the character set ---------------------------------------------------

    /// <summary>
    /// The table is checked against .NET's own CP437 decoder, an implementation
    /// written by someone else from the same published mapping, so a typo in
    /// either shows up here.
    /// </summary>
    [Fact]
    public void The_cp437_table_matches_an_independent_decoder()
    {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        var cp437 = Encoding.GetEncoding(437);

        for (int b = 0x80; b <= 0xFF; b++)
        {
            string s = cp437.GetString(new[] { (byte)b });
            Assert.Equal(1, s.Length);
            Assert.True(s[0] == CodePage437.Upper[b - 0x80],
                $"0x{b:X2}: table says U+{CodePage437.Upper[b - 0x80]:X4}, decoder says U+{(int)s[0]:X4}");
        }
        Assert.Equal(128, CodePage437.Upper.Distinct().Count());
    }

    [Fact]
    public void A_font_missing_a_character_of_the_set_is_refused_with_the_codepoints_named()
    {
        var font = BdfFont.Parse(Bdf(GlyphA, 1));
        var ex = Assert.Throws<FontNotReadyException>(() => ConsoleFont.Build(font, ConsoleFont.Charset.Cp437, "T"));

        Assert.Equal(94 + 128, ex.Missing.Count);          // everything but 'A'
        Assert.Contains(0x2500u, ex.Missing);              // box drawing, slot 0xC4
        Assert.DoesNotContain((uint)'A', ex.Missing);
        Assert.Contains("U+2500", ex.Message);
    }

    [Fact]
    public void Slots_follow_the_set_ascii_at_its_own_value_and_the_upper_half_in_cp437_order()
    {
        var slots = ConsoleFont.Slots(ConsoleFont.Charset.Cp437).ToDictionary(s => s.Slot, s => s.Codepoint);

        Assert.Equal(95 + 128, slots.Count);
        Assert.Equal((uint)'A', slots['A']);
        Assert.Equal(0x00C7u, slots[0x80]);    // Ç
        Assert.Equal(0x2500u, slots[0xC4]);    // ─
        Assert.Equal(0x2588u, slots[0xDB]);    // █
        Assert.Equal(0x00A0u, slots[0xFF]);    // no-break space
        Assert.False(slots.ContainsKey(0x7F)); // DEL is a control, never drawn
        Assert.False(slots.ContainsKey(0x0A)); // nor is newline
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


    // ---- the font the OS ships ------------------------------------------------

    /// <summary>
    /// Terminus, as vendored, converts strictly for CP437, and the converted
    /// font has the right characters in the right slots with real pixels in
    /// them. Skipped rather than failed when the assets are not beside the run.
    /// </summary>
    [Fact]
    public void The_shipped_terminus_converts_for_cp437_with_ink_in_the_right_slots()
    {
        string? bdf = FindTerminus();
        if (bdf is null) return;

        var r = ConsoleFont.Build(BdfFont.Load(bdf), ConsoleFont.Charset.Cp437, "Console");
        Assert.Equal(8, r.CellWidth);
        Assert.Equal(16, r.CellHeight);
        Assert.Equal(95 + 128, r.Placed);
        Assert.Equal("Console", r.Face);

        var h = XfntFormat.Read(r.Bytes);
        Assert.Equal(256, h.GlyphCount);
        Assert.True(h.HasCodepoints);

        var a = XfntFormat.Glyph(r.Bytes, h, 'A');
        Assert.Contains(a, b => b != 0);                 // 'A' is not blank
        Assert.DoesNotContain(a, b => b == 0xFF);        // nor a solid block
        Assert.Equal((uint)'A', XfntFormat.Codepoint(r.Bytes, h, 'A'));

        // Slot 0xDB is the full block in CP437: every pixel of every row.
        Assert.All(XfntFormat.Glyph(r.Bytes, h, 0xDB), b => Assert.Equal(0xFF, b));
        Assert.Equal(0x2588u, XfntFormat.Codepoint(r.Bytes, h, 0xDB));

        // Slot 0xC4, the horizontal line, has exactly one row of ink, full width.
        var line = XfntFormat.Glyph(r.Bytes, h, 0xC4);
        Assert.Single(line, b => b != 0);
        Assert.Contains((byte)0xFF, line);

        // A control slot is blank.
        Assert.All(XfntFormat.Glyph(r.Bytes, h, 0x0A), b => Assert.Equal(0, b));
    }

    private static string? FindTerminus()
    {
        var d = new DirectoryInfo(AppContext.BaseDirectory);
        while (d is not null)
        {
            string p = Path.Combine(d.FullName, "vendor", "terminus", "ter-u16n.bdf");
            if (File.Exists(p)) return p;
            d = d.Parent;
        }
        return null;
    }
}
