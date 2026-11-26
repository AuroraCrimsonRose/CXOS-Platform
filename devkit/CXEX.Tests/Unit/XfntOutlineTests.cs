// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using CXEX.Build.Fonts;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// XFNT v2 (roadmap 2.8a′1): the outline format's checker, the TrueType
/// converter and the oracle rasteriser.
///
/// The checker's tests are one valid file and one mutation per rule, each
/// required to fail with that rule's words, so a check that goes missing
/// turns exactly its own case red. The rasteriser's are shapes whose
/// coverage is known exactly or by area; the converter's run on the
/// vendored Plex.
/// </summary>
public class XfntOutlineTests
{
    // ---- a small valid face -------------------------------------------------
    //
    // glyph 0: a square. glyph 1 ('A', 'B'): a quadratic arch and a triangle.
    // glyph 2 (' '): no contours.
    private static OutlineFace Small()
    {
        var sq = new OGlyph { Advance = 2048 };
        sq.Contours.Add(new() { P(0, 0), P(2048, 0), P(2048, 2048), P(0, 2048) });
        var a = new OGlyph { Advance = 2560 };
        a.Contours.Add(new() { P(0, 0), new OPoint(1024, 2048, false), P(2048, 0) });
        a.Contours.Add(new() { P(512, 256), P(1536, 256), P(1024, 512) });
        var space = new OGlyph { Advance = 1024 };
        var f = new OutlineFace
        {
            Face = "Test", Weight = 400, Ascender = 3500, Descender = -900, LineGap = 100,
            CapHeight = 2800, XHeight = 2000, UnderlinePosition = -400, UnderlineThickness = 200,
        };
        f.Glyphs.AddRange(new[] { sq, a, space });
        f.Map.Add((' ', 2));
        f.Map.Add(('A', 1));
        f.Map.Add(('B', 1));
        return f;
    }

    private static OPoint P(int x, int y) => new((short)x, (short)y, true);

    // Section starts of Small(): 3 map entries, 3 glyphs, 3 contours, 10 points.
    private const int MapAt = 64, GlyphAt = MapAt + 3 * 8, ContourAt = GlyphAt + 3 * 16,
                      PointAt = ContourAt + 3 * 8, OnAt = PointAt + 10 * 4, End = OnAt + 12;

    private static void W16(byte[] f, int o, int v) { f[o] = (byte)v; f[o + 1] = (byte)(v >> 8); }
    private static void W32(byte[] f, int o, long v) { for (int i = 0; i < 4; i++) f[o + i] = (byte)(v >> (8 * i)); }

    [Fact]
    public void The_small_face_is_valid_and_reads_back_as_written()
    {
        var bytes = XfntOutline.Write(Small());
        Assert.Equal(End, bytes.Length);
        var f = XfntOutline.Read(bytes);
        Assert.Equal("Test", f.Face);
        Assert.Equal(3, f.Glyphs.Count);
        Assert.Equal(1, f.Lookup('A'));
        Assert.Equal(2, f.Lookup(' '));
        Assert.Equal(0, f.Lookup('Z'));
        Assert.False(f.Glyphs[1].Contours[0][1].OnCurve);
        Assert.Equal((short)2048, f.Glyphs[1].XMax);
        Assert.Equal((short)2048, f.Glyphs[1].YMax);
        Assert.Equal(-900, f.Descender);
    }

    public static IEnumerable<object[]> Mutations() => new (string Name, Action<byte[]> Break, string Says)[]
    {
        ("magic",            f => f[0] ^= 1,                          "bad magic"),
        ("version",          f => W16(f, 4, 1),                       "not XFNT v2"),
        ("kind",             f => f[6] = XfntFormat.KindBitmap,       "outlines only"),
        ("flags",            f => f[7] = 0x80,                        "unknown flags"),
        ("em",               f => W16(f, 8, 2048),                    "the em must be"),
        ("weight",           f => W16(f, 10, 0),                      "weight"),
        ("ascender",         f => W16(f, 12, 0),                      "ascender"),
        ("descender",        f => W16(f, 14, 5),                      "descender"),
        ("line gap",         f => W16(f, 16, 0x8000),                 "line gap"),
        ("cap height",       f => W16(f, 18, 0x9000),                 "cap or x height"),
        ("underline",        f => W16(f, 24, 0xF000),                 "underline"),
        ("reserved 26",      f => f[26] = 1,                          "reserved"),
        ("reserved 44",      f => f[44] = 1,                          "reserved"),
        ("no glyphs",        f => W32(f, 28, 0),                      "glyph count"),
        ("map count",        f => W32(f, 32, 0x110001),               "map count"),
        ("contour count",    f => W32(f, 36, (1 << 22) + 1),          "contour count"),
        ("point count",      f => W32(f, 40, (1 << 24) + 1),          "point count"),
        ("counts disagree",  f => W32(f, 40, 9),                      "the counts describe"),
        ("empty name",       f => f[48] = 0,                          "empty"),
        ("name control",     f => f[49] = 7,                          "printable"),
        ("name tail",        f => f[63] = (byte)'x',                  "NUL padded"),
        ("map order",        f => W32(f, MapAt + 16, 'A'),            "strictly ascending"),
        ("map surrogate",    f => W32(f, MapAt + 16, 0xDC00),         "not a character"),
        ("map past unicode", f => W32(f, MapAt + 16, 0x110000),       "not a character"),
        ("map glyph",        f => W32(f, MapAt + 12, 3),              "past the font"),
        ("advance",          f => W16(f, GlyphAt + 16, XfntOutline.Limit + 1), "advance"),
        ("first contour",    f => W32(f, GlyphAt + 16 + 12, 0),       "its first contour"),
        ("contours past",    f => W16(f, GlyphAt + 16 + 10, 3),       "run past the table"),
        ("contour total",    f => { W16(f, GlyphAt + 16 + 10, 1); W32(f, GlyphAt + 32 + 12, 2); }, "the glyphs hold"),
        ("box inverted",     f => W16(f, GlyphAt + 16 + 2, 3000),     "box out of range"),
        ("box past limit",   f => W16(f, GlyphAt + 16 + 6, 0x7000),   "box out of range"),
        ("empty with a box", f => W16(f, GlyphAt + 32 + 6, 10),       "box out of range"),
        ("first point",      f => W32(f, ContourAt + 8, 0),           "its first point"),
        ("one point",        f => { W32(f, ContourAt + 4, 1); W32(f, ContourAt + 8, 1); }, "fewer than two"),
        ("points past",      f => W32(f, ContourAt + 16 + 4, 4),      "run past the table"),
        ("point total",      f => W32(f, ContourAt + 16 + 4, 2),      "the contours hold"),
        ("point outside",    f => W16(f, PointAt + 2, 0xFFFF),        "outside its box"),
        ("flag value",       f => f[OnAt + 2] = 2,                    "on-curve flag"),
        ("starts off",       f => f[OnAt + 4] = 0,                    "starts off the curve"),
        ("two off",          f => f[OnAt + 6] = 0,                    "two off-curve"),
        ("padding",          f => f[OnAt + 11] = 1,                   "padding"),
        ("one byte long",    f => { },                                "the counts describe"),
    }.Select(m => new object[] { m.Name, m.Break, m.Says });

    [Theory]
    [MemberData(nameof(Mutations))]
    public void Each_rule_of_the_checker_refuses_its_own_break(string name, Action<byte[]> breakIt, string says)
    {
        var good = XfntOutline.Write(Small());
        var bad = name == "one byte long" ? good.Concat(new byte[] { 0 }).ToArray() : (byte[])good.Clone();
        breakIt(bad);
        var ex = Assert.Throws<InvalidDataException>(() => XfntOutline.Read(bad));
        Assert.Contains(says, ex.Message);
    }

    [Fact]
    public void Every_truncation_of_a_valid_file_is_refused()
    {
        var good = XfntOutline.Write(Small());
        for (int n = 0; n < good.Length; n++)
            Assert.Throws<InvalidDataException>(() => XfntOutline.Read(good[..n]));
    }

    // ---- the rasteriser -----------------------------------------------------

    private static OGlyph Poly(params (int X, int Y)[][] contours)
    {
        var g = new OGlyph { Advance = 4096 };
        foreach (var c in contours) g.Contours.Add(c.Select(p => P(p.X, p.Y)).ToList());
        XfntOutline.Box(g);
        return g;
    }

    private static (int X, int Y)[] Square(int x0, int y0, int x1, int y1, bool clockwise = true)
        => clockwise ? new[] { (x0, y0), (x0, y1), (x1, y1), (x1, y0) }
                     : new[] { (x0, y0), (x1, y0), (x1, y1), (x0, y1) };

    [Fact]
    public void A_square_of_whole_pixels_is_fully_covered_and_placed_above_the_baseline()
    {
        var b = OutlineRaster.Draw(Poly(Square(0, 0, 4096, 4096)), 8);
        Assert.Equal((0, -8, 8, 8), (b.Left, b.Top, b.Width, b.Height));
        Assert.All(b.Coverage, v => Assert.Equal(255, v));
        Assert.Equal(8 * 64, b.Advance26_6);
    }

    [Fact]
    public void A_square_inset_half_a_pixel_has_half_edges_and_quarter_corners()
    {
        // at 8 ppem a pixel is 512 em units
        var b = OutlineRaster.Draw(Poly(Square(256, 256, 4096 - 256, 4096 - 256)), 8);
        Assert.Equal(8, b.Width);
        Assert.Equal(64, b.At(0, 0));
        Assert.Equal(128, b.At(3, 0));
        Assert.Equal(128, b.At(0, 4));
        Assert.Equal(255, b.At(4, 4));
        Assert.Equal(64, b.At(7, 7));
    }

    [Fact]
    public void Two_contours_the_same_way_round_clamp_to_full_and_do_not_wrap()
    {
        var b = OutlineRaster.Draw(Poly(Square(0, 0, 4096, 4096), Square(0, 0, 4096, 4096)), 8);
        Assert.All(b.Coverage, v => Assert.Equal(255, v));
    }

    [Fact]
    public void A_contour_the_other_way_round_is_a_hole()
    {
        var b = OutlineRaster.Draw(Poly(Square(0, 0, 4096, 4096), Square(1024, 1024, 3072, 3072, clockwise: false)), 8);
        Assert.Equal(0, b.At(3, 3));
        Assert.Equal(0, b.At(4, 4));
        Assert.Equal(255, b.At(0, 0));
        Assert.Equal(255, b.At(1, 3));
    }

    [Fact]
    public void A_diagonal_edge_covers_its_area_exactly_enough()
    {
        // a right triangle of 64 x 64 pixels: 2048 square pixels
        var b = OutlineRaster.Draw(Poly(new[] { (0, 0), (0, 4096), (4096, 0) }), 64);
        double area = b.Coverage.Sum(v => (double)v) / 255;
        Assert.InRange(area, 2048 * 0.995, 2048 * 1.005);
        // the diagonal cuts its pixels in half
        Assert.InRange(b.At(10, 10), 120, 136);
    }

    [Fact]
    public void A_quadratic_covers_two_thirds_of_its_control_triangle()
    {
        // (0,0) (2048,4096) (4096,0) at 64 ppem: base 64 px, control height 64 px,
        // the parabola's area is 2/3 * 64 * 32 = 1365.3 square pixels
        var g = new OGlyph { Advance = 4096 };
        g.Contours.Add(new() { P(0, 0), new OPoint(2048, 4096, false), P(4096, 0) });
        XfntOutline.Box(g);
        var b = OutlineRaster.Draw(g, 64);
        double area = b.Coverage.Sum(v => (double)v) / 255;
        Assert.InRange(area, 1365.3 * 0.99, 1365.3 * 1.01);
    }

    /// <summary>
    /// Coverage by 16 x 16 samples a pixel, by the rasteriser's stated rule
    /// (the samples' winding numbers summed, then the magnitude clamped to
    /// a full pixel; strict non-zero differs where winding 2 meets 0), over the
    /// same scaled points the rasteriser uses: the reference for any outline
    /// of lines.
    /// </summary>
    private static double[] Sampled(OGlyph g, int ppem, GlyphBitmap b)
    {
        var polys = g.Contours.Select(c => c.Select(p =>
            (X: OutlineRaster.Scale(p.X, ppem) / 64.0 - b.Left, Y: -OutlineRaster.Scale(p.Y, ppem) / 64.0 - b.Top)).ToArray()).ToList();
        var o = new double[b.Width * b.Height];
        for (int py = 0; py < b.Height; py++)
            for (int px = 0; px < b.Width; px++)
            {
                int inside = 0;
                for (int j = 0; j < 16; j++)
                    for (int i = 0; i < 16; i++)
                    {
                        double sx = px + (i + 0.5) / 16, sy = py + (j + 0.5) / 16;
                        int wind = 0;
                        foreach (var poly in polys)
                            for (int k = 0; k < poly.Length; k++)
                            {
                                var a = poly[k]; var e = poly[(k + 1) % poly.Length];
                                if ((a.Y <= sy) == (e.Y <= sy)) continue;
                                double x = a.X + (sy - a.Y) * (e.X - a.X) / (e.Y - a.Y);
                                if (x > sx) wind += e.Y > a.Y ? 1 : -1;
                            }
                        inside += wind;
                    }
                o[py * b.Width + px] = Math.Min(255, Math.Abs(inside) * 255.0 / 256);
            }
        return o;
    }

    public static IEnumerable<object[]> Outlines() => new (string Name, (int X, int Y)[][] Contours)[]
    {
        ("a shallow edge rising right", new[] { new[] { (0, 0), (0, 1024), (4096, 0) } }),
        ("a shallow edge falling right", new[] { new[] { (0, 0), (4096, 1024), (4096, 0) } }),
        ("a steep sliver", new[] { new[] { (1900, 0), (2300, 4096), (2200, 0) } }),
        ("a star that crosses itself", new[] { new[] { (2048, 4000), (800, 100), (3900, 2600), (200, 2600), (3300, 100) } }),
        ("a frame with a slanted hole", new[] { Square(0, 0, 4096, 3000), new[] { (700, 500), (3000, 2400), (3400, 600) } }),
    }.SelectMany(o => new[] { 13, 32 }.Select(ppem => new object[] { o.Name, o.Contours, ppem }));

    [Theory]
    [MemberData(nameof(Outlines))]
    public void Every_pixel_agrees_with_a_supersampled_reference(string name, (int X, int Y)[][] contours, int ppem)
    {
        var g = Poly(contours);
        var b = OutlineRaster.Draw(g, ppem);
        var want = Sampled(g, ppem, b);
        for (int i = 0; i < want.Length; i++)
            Assert.True(Math.Abs(b.Coverage[i] - want[i]) <= 20,
                $"{name} at {ppem} ppem: pixel ({i % b.Width}, {i / b.Width}) is {b.Coverage[i]}, sampled {want[i]:F0}");
    }

    [Fact]
    public void The_scale_rounds_to_the_nearest_64th_on_both_sides_of_zero()
    {
        // 1 em unit at 64 ppem is a 64th exactly; at 1 ppem it rounds
        Assert.Equal(1, OutlineRaster.Scale(1, 64));
        Assert.Equal(-1, OutlineRaster.Scale(-1, 64));
        Assert.Equal(0, OutlineRaster.Scale(31, 1));
        Assert.Equal(1, OutlineRaster.Scale(32, 1));
        Assert.Equal(0, OutlineRaster.Scale(-32, 1));
        Assert.Equal(-1, OutlineRaster.Scale(-33, 1));
        Assert.Equal(-64 * 4 * 1024, OutlineRaster.Scale(-XfntOutline.Limit, 1024));
    }

    [Fact]
    public void A_glyph_with_no_contours_draws_nothing_but_still_advances()
    {
        var b = OutlineRaster.Draw(new OGlyph { Advance = 1024 }, 16);
        Assert.Equal(0, b.Width);
        Assert.Equal(4 * 64, b.Advance26_6);
    }

    // ---- the converter ------------------------------------------------------

    [Fact]
    public void Implied_points_are_written_out_and_a_contour_starts_on_the_curve()
    {
        // four off-curve points: TrueType's circle-ish shape with every on-curve point implied
        var src = new[] { (0.0, 100.0), (100.0, 0.0), (0.0, -100.0), (-100.0, 0.0) }
            .Select(p => new TrueTypeFont.Point(p.Item1, p.Item2, false)).ToList();
        var n = OutlineConvert.Normalise(src, v => (short)Math.Round(v))!;
        Assert.Equal(8, n.Count);
        Assert.True(n[0].OnCurve);
        for (int i = 0; i < n.Count; i++) Assert.NotEqual(n[i].OnCurve, n[(i + 1) % n.Count].OnCurve);
        Assert.Equal(new OPoint(50, 50, true), n[0]);
    }

    [Fact]
    public void A_contour_that_draws_nothing_is_dropped_and_repeated_points_go()
    {
        var one = new List<TrueTypeFont.Point> { new(5, 5, true) };
        Assert.Null(OutlineConvert.Normalise(one, v => (short)v));
        var same = new List<TrueTypeFont.Point> { new(5, 5, true), new(5, 5, true), new(5, 5, true) };
        Assert.Null(OutlineConvert.Normalise(same, v => (short)v));
        var dup = new List<TrueTypeFont.Point> { new(0, 0, true), new(0, 0, true), new(9, 0, true), new(9, 9, true), new(0, 0, true) };
        Assert.Equal(3, OutlineConvert.Normalise(dup, v => (short)v)!.Count);
    }

    [Fact]
    public void A_new_name_may_not_share_a_reserved_word_of_the_source_family()
    {
        Assert.Equal("Plex", OutlineConvert.NameClash("Plex Sans", "IBM Plex Sans"));
        Assert.Equal("Plex", OutlineConvert.NameClash("cx plex", "IBM Plex Mono"));
        Assert.Null(OutlineConvert.NameClash("CX Sans", "IBM Plex Sans"));
        Assert.Null(OutlineConvert.NameClash("CX Mono", "IBM Plex Mono"));
    }

    private static string Plex(string file)
    {
        string? root = TestEnv.RepoRoot;
        Assert.SkipWhen(root is null, "no CXOS checkout found. Set CXK_ROOT to the repository root.");
        string p = Path.Combine(root!, "vendor", "plex", file);
        Assert.True(File.Exists(p), $"{p} is vendored and must be there");
        return p;
    }

    [Fact]
    public void Plex_Sans_converts_with_its_metrics_and_its_composites_flattened()
    {
        var tt = TrueTypeFont.Load(Plex("sans/IBMPlexSans-Regular.ttf"));
        Assert.Equal("IBM Plex Sans", tt.Family);
        var f = XfntOutline.Read(XfntOutline.Write(OutlineConvert.Convert(tt, "CX Sans")));

        Assert.Equal(400, f.Weight);
        Assert.False(f.Italic);
        Assert.False(f.Monospace);
        Assert.Equal((int)Math.Round(tt.Ascender * 4096.0 / tt.UnitsPerEm, MidpointRounding.AwayFromZero), f.Ascender);
        foreach (char c in "AZaz09.,?@")
            Assert.NotEqual(0, f.Lookup(c));
        Assert.Equal(f.Glyphs.Count - 1, f.Map.Select(m => m.Glyph).Distinct().Count());

        // Á is A and an accent: more contours than A, the same advance
        var A = f.Glyphs[f.Lookup('A')];
        var Aacute = f.Glyphs[f.Lookup('Á')];
        Assert.True(Aacute.Contours.Count > A.Contours.Count);
        Assert.Equal(A.Advance, Aacute.Advance);
        Assert.True(Aacute.YMax > A.YMax);
        // the accent is placed by its component offset: wholly above the A
        Assert.Contains(Aacute.Contours, c => c.Min(p => p.Y) > A.YMax);
        // and centred over it, which only the offset's x does in Plex
        var accent = Aacute.Contours.First(c => c.Min(p => p.Y) > A.YMax);
        int mid = (accent.Min(p => p.X) + accent.Max(p => p.X)) / 2, third = (A.XMax - A.XMin) / 3;
        Assert.InRange(mid, A.XMin + third, A.XMax - third);

        // each advance is the font's own, rescaled
        int gA = tt.Map['A'];
        Assert.Equal((int)Math.Round(tt.Advance(gA) * 4096.0 / tt.UnitsPerEm, MidpointRounding.AwayFromZero), A.Advance);

        // a box is its points' own, not a looser one
        foreach (var g in f.Glyphs.Where(g => g.Contours.Count > 0))
        {
            var pts = g.Contours.SelectMany(c => c).ToList();
            Assert.Equal(pts.Min(p => p.X), g.XMin);
            Assert.Equal(pts.Max(p => p.Y), g.YMax);
        }
    }

    [Fact]
    public void Plex_Mono_is_monospace_and_every_glyph_advances_alike()
    {
        var tt = TrueTypeFont.Load(Plex("mono/IBMPlexMono-BoldItalic.ttf"));
        var f = OutlineConvert.Convert(tt, "CX Mono");
        Assert.True(f.Monospace);
        Assert.True(f.Italic);
        Assert.Equal(700, f.Weight);
        int adv = f.Glyphs[f.Lookup('M')].Advance;
        foreach (char c in "iMW0 _")
            Assert.Equal(adv, f.Glyphs[f.Lookup(c)].Advance);
    }

    [Fact]
    public void Plex_o_draws_a_ring_with_a_hole()
    {
        var f = OutlineConvert.Convert(TrueTypeFont.Load(Plex("sans/IBMPlexSans-Regular.ttf")), "CX Sans");
        var b = OutlineRaster.Draw(f.Glyphs[f.Lookup('o')], 48);
        Assert.Equal(0, b.At(b.Width / 2, b.Height / 2));
        Assert.Equal(255, b.At(1, b.Height / 2));
        Assert.Equal(255, b.At(b.Width / 2, 1));
        Assert.True(b.Top < 0 && b.Top + b.Height > 0, "an o sits on the baseline");
    }

    [Fact]
    public void Every_vendored_Plex_converts_and_passes_the_checker()
    {
        foreach (var file in new[] { "Regular", "Bold", "Italic", "BoldItalic" }
                 .SelectMany(s => new[] { $"sans/IBMPlexSans-{s}.ttf", $"mono/IBMPlexMono-{s}.ttf" }))
        {
            var f = XfntOutline.Read(XfntOutline.Write(OutlineConvert.Convert(TrueTypeFont.Load(Plex(file)), "CX Test")));
            Assert.True(f.Map.Count > 900, file);
        }
    }

    private static int TableAt(byte[] f, string tag)
    {
        int n = (f[4] << 8) | f[5];
        for (int i = 0; i < n; i++)
        {
            int r = 12 + i * 16;
            if (System.Text.Encoding.ASCII.GetString(f, r, 4) == tag)
                return (f[r + 8] << 24) | (f[r + 9] << 16) | (f[r + 10] << 8) | f[r + 11];
        }
        throw new InvalidOperationException($"no {tag}");
    }

    [Fact]
    public void A_font_whose_ascender_is_not_above_the_baseline_is_refused_as_bad_data()
    {
        // both places an ascender is read from: hhea, and OS/2's typographic one
        var b = File.ReadAllBytes(Plex("sans/IBMPlexSans-Regular.ttf"));
        int hhea = TableAt(b, "hhea"), os2 = TableAt(b, "OS/2");
        b[hhea + 4] = b[hhea + 5] = 0;
        b[os2 + 68] = b[os2 + 69] = 0;
        var ex = Assert.Throws<InvalidDataException>(() => OutlineConvert.Convert(TrueTypeFont.Parse(b), "CX Sans"));
        Assert.Contains("ascender", ex.Message);
    }

    [Fact]
    public void The_typographic_metrics_are_used_exactly_when_the_font_asks()
    {
        var b = File.ReadAllBytes(Plex("sans/IBMPlexSans-Regular.ttf"));
        int hhea = TableAt(b, "hhea"), os2 = TableAt(b, "OS/2");
        b[hhea + 4] = 900 >> 8; b[hhea + 5] = 900 & 0xFF;    // hhea ascender 900
        b[os2 + 68] = 1234 >> 8; b[os2 + 69] = 1234 & 0xFF;  // OS/2 typographic ascender 1234
        int Asc(bool typo)
        {
            if (typo) b[os2 + 63] |= 0x80; else b[os2 + 63] &= 0x7F; // fsSelection bit 7, USE_TYPO_METRICS
            return OutlineConvert.Convert(TrueTypeFont.Parse(b), "CX Sans").Ascender;
        }
        Assert.Equal((int)Math.Round(1234 * 4.096, MidpointRounding.AwayFromZero), Asc(true));
        Assert.Equal((int)Math.Round(900 * 4.096, MidpointRounding.AwayFromZero), Asc(false));
    }

    /// <summary>
    /// A TrueType of three glyphs, built here: 0 empty, 1 ('A') a 100-unit
    /// square, 2 ('B') a composite of glyph 1 at (0, 0) and glyph 1 again at
    /// (300, 50) scaled by a half. `self` makes the composite name itself.
    /// </summary>
    private static byte[] TinyTrueType(bool self = false)
    {
        static void U16(List<byte> b, int v) { b.Add((byte)(v >> 8)); b.Add((byte)v); }
        static void U32(List<byte> b, long v) { U16(b, (int)(v >> 16)); U16(b, (int)v); }

        var square = new List<byte>();
        U16(square, 1); U16(square, 0); U16(square, 0); U16(square, 100); U16(square, 100); // 1 contour, box
        U16(square, 3);                 // its last point
        U16(square, 0);                 // no instructions
        for (int i = 0; i < 4; i++) square.Add(0x01); // on curve, x and y as 16-bit deltas
        foreach (int dx in new[] { 0, 0, 100, 0 }) U16(square, dx);
        foreach (int dy in new[] { 0, 100, 0, -100 }) U16(square, dy & 0xFFFF);

        var comp = new List<byte>();
        U16(comp, 0xFFFF); U16(comp, 0); U16(comp, 0); U16(comp, 350); U16(comp, 100);
        U16(comp, 0x0001 | 0x0002 | 0x0020); U16(comp, self ? 2 : 1); U16(comp, 0); U16(comp, 0);    // words, xy, more
        U16(comp, 0x0001 | 0x0002 | 0x0008); U16(comp, 1); U16(comp, 300); U16(comp, 50); U16(comp, 0x2000); // scale 0.5

        var glyf = new List<byte>(); var loca = new List<byte>();
        U32(loca, 0); U32(loca, 0);
        glyf.AddRange(square); U32(loca, glyf.Count);
        glyf.AddRange(comp); U32(loca, glyf.Count);

        var head = new List<byte>(new byte[54]); head[18] = 1000 >> 8; head[19] = 1000 & 0xFF; head[51] = 1; // upm, long loca
        var maxp = new List<byte>(); U32(maxp, 0x00005000); U16(maxp, 3);
        var hhea = new List<byte>(new byte[36]); hhea[4] = 800 >> 8; hhea[5] = 800 & 0xFF; hhea[35] = 3;
        var hmtx = new List<byte>(); foreach (int a in new[] { 500, 600, 700 }) { U16(hmtx, a); U16(hmtx, 0); }
        var cmap = new List<byte>();
        U16(cmap, 0); U16(cmap, 1); U16(cmap, 3); U16(cmap, 1); U32(cmap, 12);
        U16(cmap, 4); U16(cmap, 32); U16(cmap, 0); U16(cmap, 4); U16(cmap, 4); U16(cmap, 1); U16(cmap, 0);
        U16(cmap, 'B'); U16(cmap, 0xFFFF); U16(cmap, 0); U16(cmap, 'A'); U16(cmap, 0xFFFF);
        U16(cmap, (1 - 'A') & 0xFFFF); U16(cmap, 1); U16(cmap, 0); U16(cmap, 0);

        var tables = new (string Tag, List<byte> Data)[]
        { ("cmap", cmap), ("glyf", glyf), ("head", head), ("hhea", hhea), ("hmtx", hmtx), ("loca", loca), ("maxp", maxp) };
        var f = new List<byte>();
        U32(f, 0x00010000); U16(f, tables.Length); U16(f, 0); U16(f, 0); U16(f, 0);
        int at = 12 + 16 * tables.Length;
        foreach (var (tag, data) in tables)
        {
            f.AddRange(System.Text.Encoding.ASCII.GetBytes(tag)); U32(f, 0); U32(f, at); U32(f, data.Count);
            at += (data.Count + 3) & ~3;
        }
        foreach (var (_, data) in tables) { f.AddRange(data); while (f.Count % 4 != 0) f.Add(0); }
        return f.ToArray();
    }

    [Fact]
    public void A_composites_components_are_moved_and_scaled_as_their_records_say()
    {
        var f = OutlineConvert.Convert(TrueTypeFont.Parse(TinyTrueType()), "CX Test");
        var b = f.Glyphs[f.Lookup('B')];
        Assert.Equal(2, b.Contours.Count);
        short Q(double v) => (short)Math.Round(v * 4.096, MidpointRounding.AwayFromZero);
        // the second copy: its square's (100, 100) corner at 300 + 50, 50 + 50
        Assert.Contains(b.Contours[1], p => p.X == Q(350) && p.Y == Q(100));
        Assert.Contains(b.Contours[1], p => p.X == Q(300) && p.Y == Q(50));
        Assert.DoesNotContain(b.Contours[1], p => p.X == 0);
        Assert.Equal(Q(700), b.Advance);
    }

    [Fact]
    public void A_composite_that_names_itself_is_refused()
    {
        var ex = Assert.Throws<InvalidDataException>(() => OutlineConvert.Convert(TrueTypeFont.Parse(TinyTrueType(self: true)), "CX Test"));
        Assert.Contains("nest deeper", ex.Message);
    }

    [Fact]
    public void A_composite_that_says_use_my_metrics_takes_its_components_advance()
    {
        // Á's own hmtx entry damaged to 1 unit: its USE_MY_METRICS component (the A) decides
        var b = File.ReadAllBytes(Plex("sans/IBMPlexSans-Regular.ttf"));
        var tt = TrueTypeFont.Parse(b);
        int gid = tt.Map[0xC1], hmtx = TableAt(b, "hmtx"), hhea = TableAt(b, "hhea");
        int nHm = (b[hhea + 34] << 8) | b[hhea + 35];
        Assert.True(gid < nHm, "Á has its own hmtx entry");
        b[hmtx + gid * 4] = 0; b[hmtx + gid * 4 + 1] = 1;
        var f = OutlineConvert.Convert(TrueTypeFont.Parse(b), "CX Sans");
        Assert.Equal(f.Glyphs[f.Lookup('A')].Advance, f.Glyphs[f.Lookup('Á')].Advance);
    }

    [Fact]
    public void A_table_offset_past_any_file_is_refused_by_its_own_rule()
    {
        var b = File.ReadAllBytes(Plex("mono/IBMPlexMono-Regular.ttf"));
        b[12 + 8] = b[12 + 9] = b[12 + 10] = b[12 + 11] = 0xFF; // the first table's offset
        var ex = Assert.Throws<InvalidDataException>(() => TrueTypeFont.Parse(b));
        Assert.Contains("past any file's length", ex.Message);
    }

    [Fact]
    public void A_CFF_font_is_refused_with_the_reason()
    {
        var otto = new byte[64];
        otto[0] = (byte)'O'; otto[1] = (byte)'T'; otto[2] = (byte)'T'; otto[3] = (byte)'O';
        var ex = Assert.Throws<InvalidDataException>(() => TrueTypeFont.Parse(otto));
        Assert.Contains("CFF", ex.Message);
    }

    [Fact]
    public void A_damaged_TrueType_fails_as_bad_data_and_never_as_a_crash()
    {
        // Truncations and single-byte damage, fixed seed: the reader and the
        // converter either succeed (the write then passes the checker) or
        // refuse with InvalidDataException; an index or overflow exception is
        // a parser bug.
        var good = File.ReadAllBytes(Plex("mono/IBMPlexMono-Regular.ttf"));
        var rng = new Random(2026_11_25 + TestEnv.Seed); // CXEX_TEST_SEED tries others
        var cases = new List<byte[]>();
        foreach (int n in new[] { 0, 11, 12, 100, 1000, good.Length / 3, good.Length / 2, good.Length - 1 })
            cases.Add(good[..n]);
        for (int i = 0; i < 300; i++)
        {
            var b = (byte[])good.Clone();
            // the table directory and the head of each table, where damage tells
            int at = i % 3 == 0 ? rng.Next(12, 12 + 16 * 18) : rng.Next(good.Length);
            b[at] = (byte)rng.Next(256);
            cases.Add(b);
        }
        foreach (var b in cases)
        {
            try { XfntOutline.Write(OutlineConvert.Convert(TrueTypeFont.Parse(b), "CX Test")); }
            catch (InvalidDataException) { }
        }
    }
}
