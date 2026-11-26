// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Build.Fonts;
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// os/std/raster.xfxn, the rasteriser the OS draws outline text with, against
/// the C# oracle (CXEX.Build/Fonts/OutlineRaster.cs) pixel for pixel
/// (system/CX_FONTS.md §4.1, roadmap 2.8a′3), as os/xc is held to the C#
/// compiler. os/xc/rastdump.xfxn draws through it and `cxk font raster`
/// through the oracle; both print a line per glyph and size (box, advance,
/// a hash of every coverage byte) and the two texts must be equal.
///
/// <para>The faces are the image's own eight, converted from the vendored
/// Plex here as the build converts them, every glyph at the sizes text is
/// read at and some at large ones; and faces generated from the seed, whose
/// glyphs are what a converter never writes: coordinates at the ±4 em limit,
/// two-point contours, overlaps that wind past one, slivers, curves whose
/// control points sit on their ends. rastdump draws them a second time
/// through the glyph cache, with a budget that flushes it and one that
/// does not, and that text must be the same again.</para>
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class RasterDiffTests
{
    // every glyph at these; a sample at the large sizes (P:S, every S-th)
    private const string RealSizes = "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,22,24,28,32,40,48,96:7,300:41,1024:211";

    [Fact]
    public void The_X_rasteriser_draws_every_glyph_as_the_oracle_does()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();

        string? osDir = HostBuild.FindOsDir(repo, "rastdump");
        Assert.SkipWhen(osDir is null, "no CXOS checkout with os/xc/rastdump.xfxn was found. Set CXK_ROOT to the repository root.");

        using var work = new HostBuild.Workspace("rastdump");
        string exe = HostBuild.BuildDumpProgram(cxk, repo, osDir!, "rastdump", work.Path);

        // the image's faces, as tools/cmake/CMakeLists.txt converts them
        var real = new List<string>();
        foreach (var (dir, face) in new[] { ("sans", "CX Sans"), ("mono", "CX Mono") })
            foreach (string ttf in Directory.EnumerateFiles(Path.Combine(repo, "vendor", "plex", dir), "*.ttf").OrderBy(p => p, StringComparer.Ordinal))
            {
                string o = Path.Combine(work.Path, Path.GetFileNameWithoutExtension(ttf) + ".xfnt");
                var b = HostBuild.Run(cxk, new[] { "font", "build", ttf, "--face", face, "-o", o }, repo);
                Assert.True(b.ExitCode == 0 && File.Exists(o), $"`cxk font build {ttf}` failed:\n{b.All}");
                real.Add(o);
            }
        Assert.Equal(8, real.Count);

        // generated faces, each at the sizes its coordinates allow (a box of
        // at most a few hundred pixels: the X side caps its cells)
        var rng = new Random(TestEnv.Seed);
        int n = Math.Max(40, TestEnv.Mutants * 4);
        var gen = new List<(string Path, string Sizes)>
        {
            (Write(work.Path, "wide", Face(rng, n, 16384)), "1,2,3,5,8,13,21"),
            (Write(work.Path, "em", Face(rng, n, 2048)), "1,4,9,16,33,64,150"),
            (Write(work.Path, "small", Face(rng, n, 300)), "7,64,333,1024"),
            // boxes past the X side's cell cap at the largest size: refused there
            // (E_NO_MEMORY), and reported so by the oracle's side, unread
            (Write(work.Path, "huge", Face(rng, 6, 16384)), "1024"),
            // the cap's edge: (2047 + 1) * 2048 cells exactly, drawn; one more column, refused
            (Write(work.Path, "edge", Edge()), "1024"),
        };

        long lines = 0;
        void Same(string what, IEnumerable<string> fonts, string sizes, params string[] cache)
        {
            var args = new List<string> { "--sizes", sizes };
            var c = HostBuild.Run(cxk, new[] { "font", "raster" }.Concat(args).Concat(fonts), repo);
            Assert.True(c.ExitCode == 0, $"`cxk font raster` exited {c.ExitCode}:\n{c.All}");
            var x = HostBuild.Run(exe, args.Concat(cache).Concat(fonts), work.Path);
            Assert.True(x.ExitCode == 0, $"rastdump exited {x.ExitCode}:\n{x.StdErr}");

            string xside = x.StdOut;
            if (TestEnv.Sabotage) xside = "sabotage\n" + xside;
            Assert.DoesNotContain("refused", c.StdOut);
            if (what == "edge.xfnt") Assert.Contains("\n1024 1 ", c.StdOut);
            if (what is "huge.xfnt" or "edge.xfnt") Assert.Contains("\nerror 1024 ", c.StdOut);
            else Assert.DoesNotContain("\nerror ", c.StdOut);
            // the cached pass asks for the glyphs scrambled: compare its lines sorted
            string csharp = cache.Length > 0 ? Sorted(c.StdOut) : c.StdOut;
            if (cache.Length > 0) xside = Sorted(xside);
            if (csharp != xside) Assert.Fail(XcDumpHarness.Describe(what, csharp, xside));
            lines += c.StdOut.Count(ch => ch == '\n');

            if (cache.Length > 0)
            {
                // the cache was asked, answered from what it kept, and (with a
                // small budget) let it go: a run that never hit compared nothing.
                // With room to spare, three runs of 64 fill it (192 of 256
                // slots) and the fourth's first glyph flushes it, so every
                // second asking is a hit: hits equal misses exactly, and a
                // glyph kept where a lookup cannot find it shows.
                var m = System.Text.RegularExpressions.Regex.Match(x.StdErr, @"cache hits (\d+) misses (\d+) flushes (\d+)");
                Assert.True(m.Success, $"rastdump printed no cache counts:\n{x.StdErr}");
                long hits = long.Parse(m.Groups[1].Value), misses = long.Parse(m.Groups[2].Value), flushes = long.Parse(m.Groups[3].Value);
                Assert.True(cache[1] == "300" ? flushes * 4 > misses : hits == misses && hits > 0, $"{what}: {m.Value}");
            }
        }

        Same("the image's faces", real, RealSizes);
        foreach (var (path, sizes) in gen) Same(Path.GetFileName(path), new[] { path }, sizes);

        // through the cache: one that holds a run of glyphs, one that flushes
        // after a glyph or two
        Same("the cache, kept", new[] { real[0], gen[1].Path }, "5,16,48", "--cache", "4000000");
        Same("the cache, flushed", new[] { real[0], gen[1].Path }, "16,48", "--cache", "300");

        Console.WriteLine($"{lines} glyph lines compared");
    }

    /// <summary>Glyph 1 at 1024 ppem is a box of exactly OutlineRaster.MaxCells; glyph 2 one column past it;
    /// glyphs 3 and 4 have a box of no width and of no height.</summary>
    private static OutlineFace Edge()
    {
        var f = new OutlineFace
        {
            Face = "Edge", Ascender = 3000, Descender = -1000, CapHeight = 2800, XHeight = 2000,
            UnderlinePosition = -300, UnderlineThickness = 200,
        };
        f.Glyphs.Add(new OGlyph { Advance = 2048 });
        foreach (int x1 in new[] { 8188, 8192 })   // a quarter pixel an em unit at 1024 ppem
        {
            var g = new OGlyph { Advance = 8192 };
            g.Contours.Add(new List<OPoint> { new(0, 0, true), new(0, 8192, true), new((short)x1, 8192, true), new((short)x1, 0, true) });
            XfntOutline.Box(g);
            f.Glyphs.Add(g);
        }
        // hairlines: a box no wide and one no high, drawn a pixel across
        foreach (var (x1, y1) in new[] { (0, 4096), (4096, 0) })
        {
            var g = new OGlyph { Advance = 4096 };
            g.Contours.Add(new List<OPoint> { new(0, 0, true), new((short)x1, (short)y1, true) });
            XfntOutline.Box(g);
            f.Glyphs.Add(g);
        }
        f.Map.Add((0x41, 1));
        return f;
    }

    private static string Sorted(string text)
        => string.Concat(text.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                             .OrderBy(l => l, StringComparer.Ordinal).Select(l => l + "\n"));

    private static string Write(string dir, string name, OutlineFace f)
    {
        string p = Path.Combine(dir, name + ".xfnt");
        File.WriteAllBytes(p, XfntOutline.Write(f));
        return p;
    }

    /// <summary>
    /// A face of <paramref name="glyphs"/> glyphs whose points lie within
    /// ±<paramref name="span"/> em units, each one of the shapes below.
    /// </summary>
    private static OutlineFace Face(Random rng, int glyphs, int span)
    {
        var f = new OutlineFace
        {
            Face = "Random", Ascender = 3000, Descender = -1000, CapHeight = 2800, XHeight = 2000,
            UnderlinePosition = -300, UnderlineThickness = 200,
        };
        f.Glyphs.Add(new OGlyph { Advance = 2048 });      // glyph 0: empty
        while (f.Glyphs.Count < glyphs)
        {
            var g = new OGlyph { Advance = rng.Next(0, XfntOutline.Limit + 1) };
            int contours = rng.Next(1, 6);
            int kind = rng.Next(6);
            for (int k = 0; k < contours; k++)
                g.Contours.Add(Contour(rng, span, kind));
            XfntOutline.Box(g);
            f.Glyphs.Add(g);
        }
        f.Map.Add((0x41, 1));
        return f;
    }

    private static List<OPoint> Contour(Random rng, int span, int kind)
    {
        short C() => (short)rng.Next(-span, span + 1);
        var c = new List<OPoint>();
        switch (kind)
        {
            case 0:   // two points: a line out and back, which covers nothing
                c.Add(new OPoint(C(), C(), true));
                c.Add(new OPoint(C(), C(), rng.Next(2) == 0));
                break;
            case 1:   // an axis-aligned box, either way round (overlaps wind past one)
            {
                short x0 = C(), y0 = C(), x1 = C(), y1 = C();
                var pts = new[] { (x0, y0), (x0, y1), (x1, y1), (x1, y0) };
                if (rng.Next(2) == 0) Array.Reverse(pts);
                foreach (var (x, y) in pts) c.Add(new OPoint(x, y, true));
                break;
            }
            case 2:   // a sliver: points within a few units of a line
            {
                short x = C(), y = C();
                int dx = rng.Next(-span, span + 1) / 2, dy = rng.Next(-span, span + 1) / 2;
                for (int i = 0; i < rng.Next(3, 7); i++)
                    c.Add(new OPoint(Clamp(x + dx * i / 6 + rng.Next(-3, 4), span), Clamp(y + dy * i / 6 + rng.Next(-3, 4), span), true));
                break;
            }
            case 3:   // a curve whose control point sits on an end, or on both
            {
                short x = C(), y = C();
                c.Add(new OPoint(x, y, true));
                c.Add(rng.Next(2) == 0 ? new OPoint(x, y, false) : new OPoint(C(), C(), false));
                c.Add(new OPoint(C(), C(), true));
                c.Add(new OPoint(C(), C(), false));
                break;
            }
            default:  // anything: on and off points, never two off together
            {
                int m = rng.Next(2, 24);
                bool prevOff = true;   // the first is on the curve
                for (int i = 0; i < m; i++)
                {
                    bool off = !prevOff && rng.Next(2) == 0;
                    c.Add(new OPoint(C(), C(), !off));
                    prevOff = off;
                }
                break;
            }
        }
        return c;
    }

    private static short Clamp(int v, int span) => (short)Math.Clamp(v, -span, span);
}
