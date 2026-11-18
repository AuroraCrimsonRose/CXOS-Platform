// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using CXEX.Build.Pictures;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// `cxk picture`: the JPEG reader, the metadata stripper, the scaler and the
/// QOI writer that turn the desktop's background into what the OS reads.
///
/// The decoder is held to an independent one: the fixtures in
/// devkit/tests/pictures and the samples of the real backgrounds are
/// ffmpeg's decode. Two decoders differ by rounding in the inverse DCT, and
/// for subsampled chroma by how they upsample it (ffmpeg interpolates, this
/// repeats), so the comparisons carry a tolerance; a wrong table, a wrong
/// zig-zag or a missed restart is off by tens to hundreds, far outside it.
/// </summary>
public class PictureTests
{
    private static string? Dir(params string[] parts)
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) return null;
        string p = Path.Combine(new[] { repo }.Concat(parts).ToArray());
        return Directory.Exists(p) || File.Exists(p) ? p : null;
    }

    private static int MaxDiff(Picture pic, byte[] expected)
    {
        Assert.Equal(expected.Length, pic.Rgb.Length);
        int max = 0;
        for (int i = 0; i < expected.Length; i++) max = Math.Max(max, Math.Abs(pic.Rgb[i] - expected[i]));
        return max;
    }

    // ---- JPEG ----

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("test444", 37, 29, 3)]     // odd size: partial blocks at both edges
    [InlineData("grad420", 48, 32, 8)]     // 4:2:0: chroma upsampled differently
    public void A_fixture_decodes_as_ffmpeg_decodes_it(string name, int w, int h, int tolerance)
    {
        string? dir = Dir("devkit", "tests", "pictures");
        if (dir is null) { Assert.True(true, "no checkout: skipped"); return; }
        var pic = Jpeg.Decode(File.ReadAllBytes(Path.Combine(dir, name + ".jpg")));
        Assert.Equal(w, pic.Width);
        Assert.Equal(h, pic.Height);
        Assert.InRange(MaxDiff(pic, File.ReadAllBytes(Path.Combine(dir, name + ".rgb"))), 0, tolerance);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_backgrounds_decode_as_ffmpeg_decodes_them()
    {
        // mountain_dawn and paper_fan_dark are 4:4:4, paper_fan_dark with a
        // restart interval; paper_fan is 4:2:0.
        string? samples = Dir("devkit", "tests", "pictures", "assets.samples");
        string? pictures = Dir("assets", "image", "background");
        if (samples is null || pictures is null) { Assert.True(true, "no checkout: skipped"); return; }
        var decoded = new Dictionary<string, Picture>();
        int checkedCount = 0;
        foreach (var line in File.ReadAllLines(samples))
        {
            if (line.StartsWith('#') || line.Trim().Length == 0) continue;
            var f = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (!decoded.TryGetValue(f[0], out var pic))
                decoded[f[0]] = pic = Jpeg.Decode(File.ReadAllBytes(Path.Combine(pictures, f[0] + ".jpg")));
            var (r, g, b) = pic.At(int.Parse(f[1]), int.Parse(f[2]));
            int tol = f[0] == "paper_fan" ? 8 : 3;
            Assert.True(Math.Abs(r - int.Parse(f[3])) <= tol && Math.Abs(g - int.Parse(f[4])) <= tol && Math.Abs(b - int.Parse(f[5])) <= tol,
                $"{f[0]} at ({f[1]},{f[2]}): got {r} {g} {b}, ffmpeg {f[3]} {f[4]} {f[5]}");
            checkedCount++;
        }
        Assert.Equal(48, checkedCount);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData(0xC2, "progressive")]
    [InlineData(0xC9, "arithmetic")]
    [InlineData(0xC3, "lossless")]
    public void A_JPEG_that_is_not_baseline_is_refused_by_name(byte sof, string named)
    {
        byte[] data = { 0xFF, 0xD8, 0xFF, sof, 0x00, 0x11, 8, 0, 16, 0, 16, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0, 0xFF, 0xD9 };
        var ex = Assert.Throws<JpegNotSupportedException>(() => Jpeg.Decode(data));
        Assert.Contains(named, ex.Message);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_cut_JPEG_is_refused_not_read_past()
    {
        string? dir = Dir("devkit", "tests", "pictures");
        if (dir is null) { Assert.True(true, "no checkout: skipped"); return; }
        byte[] data = File.ReadAllBytes(Path.Combine(dir, "test444.jpg"));
        var segs = Jpeg.Segments(data);
        int scan = segs.Last().Offset;
        // Every cut inside the headers is refused with InvalidDataException,
        // never an index out of range.
        for (int n = 3; n < scan; n += 7)
            Assert.ThrowsAny<InvalidDataException>(() => Jpeg.Decode(data[..n]));
    }

    // ---- metadata ----

    private static byte[] Segment(byte marker, byte[] body)
    {
        int len = body.Length + 2;
        return new byte[] { 0xFF, marker, (byte)(len >> 8), (byte)len }.Concat(body).ToArray();
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Stripping_drops_EXIF_XMP_IPTC_and_comments_and_copies_everything_else()
    {
        string? dir = Dir("devkit", "tests", "pictures");
        if (dir is null) { Assert.True(true, "no checkout: skipped"); return; }
        // ffmpeg writes its own comment: start from the fixture without it.
        byte[] clean = Jpeg.Strip(File.ReadAllBytes(Path.Combine(dir, "test444.jpg")), out _);
        byte[] icc = Segment(0xE2, "ICC_PROFILE\0\x01\x01profile"u8.ToArray());
        byte[] withMeta = clean[..2]
            .Concat(Segment(0xE1, "Exif\0\0GPSLatitude 51.5"u8.ToArray()))
            .Concat(icc)
            .Concat(Segment(0xED, "Photoshop 3.0\0IPTC"u8.ToArray()))
            .Concat(Segment(0xFE, "a comment"u8.ToArray()))
            .Concat(clean[2..]).ToArray();

        byte[] stripped = Jpeg.Strip(withMeta, out var dropped);

        Assert.Equal(new[] { "APP1", "APP13", "COM" }, dropped);
        Assert.Equal(clean[..2].Concat(icc).Concat(clean[2..]).ToArray(), stripped);
        Assert.Empty(Jpeg.Segments(stripped).Where(s => Jpeg.IsMetadata(s.Marker)));
        Assert.Equal(Jpeg.Decode(clean).Rgb, Jpeg.Decode(stripped).Rgb);
        Assert.Equal(stripped, Jpeg.Strip(stripped, out var again));
        Assert.Empty(again);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_backgrounds_in_the_tree_carry_no_metadata()
    {
        string? pictures = Dir("assets", "image", "background");
        if (pictures is null) { Assert.True(true, "no checkout: skipped"); return; }
        var files = Directory.GetFiles(pictures, "*.jpg");
        Assert.NotEmpty(files);
        foreach (var f in files)
            Assert.True(!Jpeg.Segments(File.ReadAllBytes(f)).Any(s => Jpeg.IsMetadata(s.Marker)),
                $"{Path.GetFileName(f)} carries metadata: run `cxk picture strip` on it");
    }

    // ---- scaling ----

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData(4288, 2848, 1920, 1080, 1920, 1275)]   // the default: width meets the screen
    [InlineData(3000, 3000, 1920, 1080, 1920, 1920)]   // square: width meets it
    [InlineData(1000, 4000, 1920, 1080, 1000, 4000)]   // narrower than the screen: never enlarged...
    [InlineData(800, 600, 1920, 1080, 800, 600)]       // ...nor a small one
    [InlineData(10000, 2000, 1920, 1080, 5400, 1080)]  // wide: height meets it
    public void The_stored_size_just_covers_the_screen(int sw, int sh, int w, int h, int ew, int eh)
    {
        Assert.Equal((ew, eh), PictureScale.CoverSize(sw, sh, w, h));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Downscaling_averages_the_area_each_pixel_covers()
    {
        // 4x2 -> 2x1: each output is the mean of a 2x2 block.
        var src = new Picture(4, 2, new byte[]
        {
            0,0,0,  100,0,0,  10,20,30,  10,20,30,
            0,0,0,  100,0,0,  10,20,30,  50,60,70,
        });
        var o = PictureScale.Resize(src, 2, 1);
        Assert.Equal(new byte[] { 50, 0, 0, 20, 30, 40 }, o.Rgb);
        // 3 -> 2: the middle source pixel is split between both outputs.
        var row = PictureScale.Resize(new Picture(3, 1, new byte[] { 0,0,0, 90,90,90, 180,180,180 }), 2, 1);
        Assert.Equal(new byte[] { 30, 30, 30, 150, 150, 150 }, row.Rgb);
        Assert.Throws<ArgumentException>(() => PictureScale.Resize(src, 5, 2));
    }

    // ---- QOI ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void QOI_chunks_are_the_specifications()
    {
        // One pixel per chunk kind: RGB, a run, DIFF, LUMA, INDEX.
        var pic = new Picture(6, 1, new byte[]
        {
            10, 20, 30,      // RGB (too far from black)
            10, 20, 30,      // run of 1
            11, 19, 31,      // DIFF +1 -1 +1
            21, 29, 41,      // LUMA dg=+10, dr-dg=0, db-dg=0
            10, 20, 30,      // INDEX of the first
            10, 20, 30,      // run of 1
        });
        byte[] q = Qoi.Encode(pic);
        byte[] header = "qoif"u8.ToArray().Concat(new byte[] { 0, 0, 0, 6, 0, 0, 0, 1, 3, 0 }).ToArray();
        int h = (10 * 3 + 20 * 5 + 30 * 7 + 255 * 11) % 64;
        byte[] body =
        {
            0xFE, 10, 20, 30,
            0xC0,
            (byte)(0x40 | (3 << 4) | (1 << 2) | 3),
            (byte)(0x80 | (10 + 32)), (byte)((8 << 4) | 8),
            (byte)h,
            0xC0,
        };
        Assert.Equal(header.Concat(body).Concat(Qoi.End).ToArray(), q);
        Assert.Equal(pic.Rgb, Qoi.Decode(q).Rgb);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void QOI_round_trips_runs_longer_than_a_chunk_and_noise()
    {
        var rnd = new Random(1);
        int w = 97, h = 13;
        var px = new byte[w * h * 3];
        for (int i = 0; i < px.Length; i++) px[i] = (byte)(i < 300 * 3 ? 0 : i < 600 * 3 ? 200 : rnd.Next(256));
        var pic = new Picture(w, h, px);
        Assert.Equal(px, Qoi.Decode(Qoi.Encode(pic)).Rgb);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_bad_QOI_is_refused()
    {
        var q = Qoi.Encode(new Picture(2, 2, new byte[] { 1, 2, 3, 40, 50, 60, 70, 80, 90, 100, 110, 120 }));
        Assert.Throws<InvalidDataException>(() => Qoi.Decode(q[..^9]));               // cut short
        var bad = (byte[])q.Clone(); bad[0] = (byte)'x';
        Assert.Throws<InvalidDataException>(() => Qoi.Decode(bad));                    // magic
        bad = (byte[])q.Clone(); bad[7] = 0;
        Assert.Throws<InvalidDataException>(() => Qoi.Decode(bad));                    // width 0
        bad = (byte[])q.Clone(); bad[^1] = 0;
        Assert.Throws<InvalidDataException>(() => Qoi.Decode(bad));                    // end marker
    }
}
