// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading;
using CXEX.Build.Fonts;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// `cxk font build` - a published BDF font -> a console XFNT v1 for one standard
/// character set.
///
/// CXOS uses existing fonts rather than drawing its own (decided 2026-10-16);
/// BDF is the source format they are published in. The conversion is strict:
/// a font that lacks a character of the set fails with the missing codepoints
/// named, and a glyph that does not fit the cell is refused rather than
/// cropped.
///
/// A TrueType source becomes an XFNT v2 outline font instead (roadmap
/// 2.8a′1): converted here so the OS never parses TrueType, under a face name
/// of its own, and read back through the same checks the OS makes.
/// </summary>
public class FontBuildCommand : Command<FontBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCE>")]
        [Description("The font as its publisher ships it: a BDF, or a TrueType .ttf")]
        public string Bdf { get; set; } = string.Empty;

        [CommandOption("-o|--out")]
        [Description("Output .xfnt. Default: the source's name with .xfnt")]
        public string? Out { get; set; }

        [CommandOption("--charset")]
        [Description("The standard character set the console's bytes mean: cp437 (default)")]
        [DefaultValue("cp437")]
        public string Charset { get; set; } = "cp437";

        [CommandOption("--face")]
        [Description("The face name stored in the font: at most 8 characters for a BDF (default Console), 16 for a TrueType (required). Never the source font's own name: OFL fonts reserve it for unmodified copies, and a name sharing a word of the source's family is refused")]
        public string? Face { get; set; }

        [CommandOption("--check")]
        [Description("Report only; write nothing")]
        public bool Check { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.Bdf))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] no such file: {Markup.Escape(settings.Bdf)}");
            return 1;
        }
        string ext = Path.GetExtension(settings.Bdf).ToLowerInvariant();
        if (ext is ".ttf" or ".otf") return BuildOutline(settings);

        if (!Enum.TryParse<ConsoleFont.Charset>(settings.Charset, true, out var set))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --charset must be cp437; got '{Markup.Escape(settings.Charset)}'");
            return 1;
        }

        ConsoleFont.Result result;
        try
        {
            var font = BdfFont.Load(settings.Bdf);
            result = ConsoleFont.Build(font, set, settings.Face ?? "Console");
        }
        catch (FontNotReadyException nr)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(nr.Message)}");
            return 1;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]{Markup.Escape(result.Face)}[/]  {result.CellWidth}x{result.CellHeight}  " +
                               $"{result.Placed} characters of {set.ToString().ToLowerInvariant()}, {result.Bytes.Length} bytes");

        if (settings.Check)
        {
            AnsiConsole.MarkupLine("[grey]--check: nothing written.[/]");
            return 0;
        }

        string outPath = settings.Out ?? Path.ChangeExtension(settings.Bdf, ".xfnt");
        try
        {
            File.WriteAllBytes(outPath, result.Bytes);
            // Read it straight back through the validating reader. The host must
            // not be able to emit a font the kernel would refuse - that failure
            // would land at boot, as far as possible from the command that
            // caused it.
            var check = XfntFormat.Read(File.ReadAllBytes(outPath));
            if (check.Width != result.CellWidth || check.Height != result.CellHeight)
                throw new InvalidDataException("the font read back with different geometry");
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] wrote [cyan]{Markup.Escape(outPath)}[/]");
        return 0;
    }

    /// <summary>TrueType to a v2 outline XFNT (roadmap 2.8a′1).</summary>
    private static int BuildOutline(Settings settings)
    {
        if (string.IsNullOrEmpty(settings.Face))
        {
            AnsiConsole.MarkupLine("[red]error:[/] a converted TrueType needs a name of its own: --face \"CX Sans\" or the like");
            return 1;
        }
        byte[] bytes;
        OutlineFace face;
        string family;
        try
        {
            var tt = TrueTypeFont.Load(settings.Bdf);
            family = tt.Family;
            string? clash = OutlineConvert.NameClash(settings.Face, tt.Family);
            if (clash is not null)
            {
                AnsiConsole.MarkupLine($"[red]error:[/] --face '{Markup.Escape(settings.Face)}' shares \"{Markup.Escape(clash)}\" with the source's family " +
                                       $"'{Markup.Escape(tt.Family)}'; an OFL Reserved Font Name may not go on a converted font");
                return 1;
            }
            face = OutlineConvert.Convert(tt, settings.Face);
            bytes = XfntOutline.Write(face); // reads itself back through the checker
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]{Markup.Escape(face.Face)}[/] from {Markup.Escape(family)}  weight {face.Weight}" +
                               $"{(face.Italic ? " italic" : "")}{(face.Monospace ? " monospace" : "")}  " +
                               $"{face.Map.Count} characters, {face.Glyphs.Count} glyphs, {bytes.Length} bytes");
        if (settings.Check)
        {
            AnsiConsole.MarkupLine("[grey]--check: nothing written.[/]");
            return 0;
        }
        string outPath = settings.Out ?? Path.ChangeExtension(settings.Bdf, ".xfnt");
        try
        {
            File.WriteAllBytes(outPath, bytes);
            XfntOutline.Read(File.ReadAllBytes(outPath));
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }
        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] wrote [cyan]{Markup.Escape(outPath)}[/]");
        return 0;
    }
}

/// <summary>`cxk font inspect` - what is actually in an .xfnt.</summary>
public class FontInspectCommand : Command<FontInspectCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<XFNT>")]
        [Description("The .xfnt to inspect")]
        public string Path { get; set; } = string.Empty;

        [CommandOption("--show")]
        [Description("Also print this slot's glyph as ASCII art")]
        [DefaultValue(-1)]
        public int Show { get; set; } = -1;

        [CommandOption("--text")]
        [Description("v2: draw this text in the terminal through the oracle rasteriser")]
        public string? Text { get; set; }

        [CommandOption("--size")]
        [Description("v2: the size --text is drawn at, in pixels per em")]
        [DefaultValue(24)]
        public int Size { get; set; } = 24;
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        try
        {
            var head = File.ReadAllBytes(settings.Path);
            if (head.Length >= 6 && BitConverter.ToUInt16(head, 4) == XfntOutline.Version)
                return InspectOutline(head, settings);
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }

        byte[] file;
        XfntFormat.Header h;
        try
        {
            file = File.ReadAllBytes(settings.Path);
            h = XfntFormat.Read(file);
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {ex.Message}");
            return 1;
        }

        int used = 0;
        for (int s = 0; s < h.GlyphCount; s++)
            if (XfntFormat.Glyph(file, h, s).Any(b => b != 0)) used++;

        AnsiConsole.MarkupLine($"[green]{(h.Face.Length > 0 ? h.Face : "(unnamed)")}[/]  XFNT v{XfntFormat.Version}");
        AnsiConsole.MarkupLine($"  cell        {h.Width}x{h.Height}  ({h.BytesPerRowValue} byte/row, {h.BytesPerGlyphValue} byte/glyph)");
        AnsiConsole.MarkupLine($"  slots       {h.GlyphCount}, [cyan]{used}[/] with ink");
        AnsiConsole.MarkupLine($"  codepoints  {(h.HasCodepoints ? "yes" : "no")}");

        if (settings.Show >= 0)
        {
            if (settings.Show >= h.GlyphCount)
            {
                AnsiConsole.MarkupLine($"[red]error:[/] slot {settings.Show} is past the {h.GlyphCount} this font has");
                return 1;
            }
            uint cp = XfntFormat.Codepoint(file, h, settings.Show);
            AnsiConsole.MarkupLine("");
            AnsiConsole.MarkupLine($"slot {settings.Show}" + (cp != 0 ? $" = U+{cp:X4}" : ""));
            var g = XfntFormat.Glyph(file, h, settings.Show);
            for (int row = 0; row < h.Height; row++)
            {
                var sb = new System.Text.StringBuilder();
                for (int col = 0; col < h.Width; col++)
                {
                    bool on = (g[row * h.BytesPerRowValue + (col >> 3)] & (0x80 >> (col & 7))) != 0;
                    sb.Append(on ? "##" : "..");
                }
                AnsiConsole.MarkupLine($"  [grey]{sb}[/]");
            }
        }
        return 0;
    }

    private static int InspectOutline(byte[] file, Settings settings)
    {
        OutlineFace f;
        try { f = XfntOutline.Read(file); }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }
        int contours = f.Glyphs.Sum(g => g.Contours.Count), points = f.Glyphs.Sum(g => g.PointCount);
        AnsiConsole.MarkupLine($"[green]{Markup.Escape(f.Face)}[/]  XFNT v{XfntOutline.Version} (outline)  weight {f.Weight}" +
                               $"{(f.Italic ? " italic" : "")}{(f.Monospace ? " monospace" : "")}");
        AnsiConsole.MarkupLine($"  em          {XfntOutline.Em}; ascender {f.Ascender}, descender {f.Descender}, gap {f.LineGap}");
        AnsiConsole.MarkupLine($"  heights     cap {f.CapHeight}, x {f.XHeight}; underline {f.UnderlinePosition} / {f.UnderlineThickness}");
        AnsiConsole.MarkupLine($"  glyphs      {f.Glyphs.Count}, {contours} contours, {points} points");
        AnsiConsole.MarkupLine($"  characters  {f.Map.Count}" + (f.Map.Count > 0 ? $" (U+{f.Map[0].Codepoint:X4} to U+{f.Map[^1].Codepoint:X4})" : ""));

        if (settings.Text is null) return 0;
        if (settings.Size is < 1 or > OutlineRaster.MaxPpem)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --size must be 1-{OutlineRaster.MaxPpem}");
            return 1;
        }
        // one line of text into a coverage buffer, the pen in 26.6
        int ppem = settings.Size;
        int asc = (int)((OutlineRaster.Scale(f.Ascender, ppem) + 63) >> 6);
        int desc = (int)((OutlineRaster.Scale(-f.Descender, ppem) + 63) >> 6);
        int height = asc + desc;
        var drawn = new List<(int X, GlyphBitmap B)>();
        long pen = 0;
        foreach (var rune in settings.Text.EnumerateRunes())
        {
            var b = OutlineRaster.Draw(f.Glyphs[f.Lookup((uint)rune.Value)], ppem);
            drawn.Add(((int)(pen >> 6), b));
            pen += b.Advance26_6;
        }
        int width = (int)((pen + 63) >> 6) + 2;
        var buf = new int[width * height];
        foreach (var (x, b) in drawn)
            for (int y = 0; y < b.Height; y++)
                for (int c = 0; c < b.Width; c++)
                {
                    int px = x + b.Left + c, py = asc + b.Top + y;
                    if (px < 0 || px >= width || py < 0 || py >= height) continue;
                    buf[py * width + px] = Math.Min(255, buf[py * width + px] + b.At(c, y));
                }
        const string shades = " .:-=+*#%@";
        AnsiConsole.WriteLine();
        for (int y = 0; y < height; y++)
        {
            var sb = new System.Text.StringBuilder(width);
            for (int x = 0; x < width; x++) sb.Append(shades[buf[y * width + x] * (shades.Length - 1) / 255]);
            AnsiConsole.WriteLine(sb.ToString().TrimEnd());
        }
        return 0;
    }
}

/// <summary>
/// `cxk font raster` - every glyph of v2 fonts through the oracle
/// rasteriser, one line each: the C# side of the Differential fact RasterDiff,
/// which runs os/xc/rastdump.xfxn (std/raster.xfxn) on the same files and
/// requires the same text. The format is rastdump's.
/// </summary>
public class FontRasterCommand : Command<FontRasterCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<XFNT>")]
        [Description("The v2 .xfnt files to draw")]
        public string[] Paths { get; set; } = Array.Empty<string>();

        [CommandOption("--sizes")]
        [Description("Sizes in pixels per em, each P or P:S for every S-th glyph: 1,12,300:17")]
        public string Sizes { get; set; } = "";
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        var sizes = new System.Collections.Generic.List<(int Ppem, int Step)>();
        foreach (string part in settings.Sizes.Split(','))
        {
            string[] ps = part.Split(':');
            if (ps.Length > 2 || !int.TryParse(ps[0], out int p) || p < 0
                || (ps.Length == 2 && (!int.TryParse(ps[1], out int s0) || s0 < 1)))
            {
                Console.Error.WriteLine("error: bad --sizes");
                return 2;
            }
            sizes.Add((p, ps.Length == 2 ? int.Parse(ps[1]) : 1));
        }
        if (settings.Paths.Length == 0) { Console.Error.WriteLine("error: no fonts"); return 2; }

        var o = new System.Text.StringBuilder();
        var stdout = Console.OpenStandardOutput();
        void Flush() { var b = System.Text.Encoding.ASCII.GetBytes(o.ToString()); stdout.Write(b, 0, b.Length); o.Clear(); }
        foreach (string path in settings.Paths)
        {
            o.Append("== ").Append(path).Append('\n');
            OutlineFace f;
            try { f = XfntOutline.Read(File.ReadAllBytes(path)); }
            catch (InvalidDataException) { o.Append("refused\n"); continue; }
            o.Append("glyphs ").Append(f.Glyphs.Count).Append('\n');
            foreach (var (ppem, step) in sizes)
                for (int g = 0; g < f.Glyphs.Count; g += step)
                {
                    // rastdump's codes for what it refuses: a size out of range
                    // (E_INVALID_ARGS) or a box past its cell cap (E_NO_MEMORY)
                    if (ppem is < 1 or > OutlineRaster.MaxPpem) { o.Append($"error {ppem} {g} -3\n"); continue; }
                    if (f.Glyphs[g].Contours.Count > 0)
                    {
                        var (_, _, bw, bh) = OutlineRaster.Box(f.Glyphs[g], ppem);
                        if ((long)(bw + 1) * bh > OutlineRaster.MaxCells) { o.Append($"error {ppem} {g} -8\n"); continue; }
                    }
                    var b = OutlineRaster.Draw(f.Glyphs[g], ppem);
                    uint h = 0x811C9DC5;
                    foreach (byte v in b.Coverage) h = (h ^ v) * 16777619;
                    o.Append(ppem).Append(' ').Append(g).Append(' ').Append(b.Advance26_6).Append(' ')
                     .Append(b.Left).Append(' ').Append(b.Top).Append(' ').Append(b.Width).Append(' ')
                     .Append(b.Height).Append(' ').Append(h.ToString("x8")).Append('\n');
                    if (o.Length > 1 << 20) Flush();
                }
        }
        Flush();
        return 0;
    }
}
