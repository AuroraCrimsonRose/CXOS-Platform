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
/// `cxk font build` - BMFont descriptor + atlas -> XFNT.
///
/// It doubles as a checker. A glyph that does not fit the cell is refused and
/// named, so running this against a font still being drawn prints exactly the
/// list of glyphs left to adjust instead of quietly cropping them.
/// </summary>
public class FontBuildCommand : Command<FontBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<FNT>")]
        [Description("BMFont text descriptor (.fnt). Its page-0 PNG is read from the same directory")]
        public string Fnt { get; set; } = string.Empty;

        [CommandOption("-o|--out")]
        [Description("Output .xfnt. Default: the descriptor's name with .xfnt")]
        public string? Out { get; set; }

        [CommandOption("-w|--width")]
        [Description("Cell width in pixels. Default: the most common glyph width")]
        [DefaultValue(0)]
        public int Width { get; set; }

        [CommandOption("-h|--height")]
        [Description("Cell height in pixels. Default: the descriptor's lineHeight")]
        [DefaultValue(0)]
        public int Height { get; set; }

        [CommandOption("-t|--threshold")]
        [Description("Ink threshold, 1-255. A pixel at or above this is on")]
        [DefaultValue(128)]
        public int Threshold { get; set; } = 128;

        [CommandOption("--ink")]
        [Description("Which pixels are glyph: auto (default, measured), light (white on black), dark (black on white), alpha")]
        [DefaultValue("auto")]
        public string Ink { get; set; } = "auto";

        [CommandOption("--allow-unfit")]
        [Description("Write the font anyway, leaving glyphs that do not fit the cell blank")]
        public bool AllowUnfit { get; set; }

        [CommandOption("--check")]
        [Description("Report only; write nothing")]
        public bool Check { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.Fnt))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] no such file: {settings.Fnt}");
            return 1;
        }
        if (settings.Threshold is < 1 or > 255)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --threshold must be 1-255, got {settings.Threshold}");
            return 1;
        }

        BmFont font;
        PngImage atlas;
        string pagePath;
        try
        {
            font = BmFont.Load(settings.Fnt);
            string dir = Path.GetDirectoryName(Path.GetFullPath(settings.Fnt)) ?? ".";
            pagePath = Path.Combine(dir, font.PageFile);
            if (!File.Exists(pagePath))
            {
                AnsiConsole.MarkupLine($"[red]error:[/] the descriptor names page file '{font.PageFile}', which is not beside it.");
                AnsiConsole.MarkupLine("[grey]  A .fnt carries no pixels; the glyphs are in that PNG.[/]");
                return 1;
            }
            atlas = PngImage.Load(pagePath);
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {ex.Message}");
            return 1;
        }

        if (!Enum.TryParse<FontAssembler.InkPolarity>(settings.Ink, true, out var polarity))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --ink must be auto, light, dark or alpha; got '{settings.Ink}'");
            return 1;
        }

        FontAssembler.Result result;
        try
        {
            result = FontAssembler.Build(font, atlas, settings.Width, settings.Height,
                                         settings.Threshold, strict: !settings.AllowUnfit,
                                         polarity: polarity);
        }
        catch (FontNotReadyException nr)
        {
            AnsiConsole.MarkupLine($"[yellow]{font.Face}[/]: [red]{nr.Rejected.Count} glyph(s) do not fit the {nr.CellWidth}x{nr.CellHeight} cell[/]");
            AnsiConsole.MarkupLine("");
            var byReason = nr.Rejected.GroupBy(r => r.Why).OrderByDescending(g => g.Count());
            foreach (var grp in byReason)
            {
                AnsiConsole.MarkupLine($"  [red]{grp.Count()}[/] x {Markup.Escape(grp.Key)}");
                var cps = grp.OrderBy(r => r.Codepoint).Select(r => $"U+{r.Codepoint:X4}");
                AnsiConsole.MarkupLine($"    [grey]{string.Join(" ", cps)}[/]");
            }
            AnsiConsole.MarkupLine("");
            AnsiConsole.MarkupLine("[grey]These are the glyphs still to adjust. Re-run when they are the cell size,[/]");
            AnsiConsole.MarkupLine("[grey]or pass --allow-unfit to write the font with those slots left blank.[/]");
            return 1;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {ex.Message}");
            return 1;
        }

        int extras = result.Assignments.Count(a => a.Slot >= FontAssembler.ExtraFirstSlot);
        int ascii = result.Placed - extras;

        AnsiConsole.MarkupLine($"[green]{font.Face}[/]  {result.CellWidth}x{result.CellHeight}  [grey]from {Path.GetFileName(pagePath)} ({atlas.Width}x{atlas.Height}, ink={result.Polarity.ToString().ToLowerInvariant()}{(settings.Ink == "auto" ? " measured" : " forced")})[/]");
        AnsiConsole.MarkupLine($"  glyphs      [cyan]{result.Placed}[/]  ({ascii} ASCII at their own slots, {extras} extras from slot {FontAssembler.ExtraFirstSlot})");
        AnsiConsole.MarkupLine($"  size        {result.Bytes.Length} bytes");
        if (result.Rejected.Count > 0)
            AnsiConsole.MarkupLine($"  [yellow]left blank  {result.Rejected.Count} glyph(s) that did not fit[/]");

        if (settings.Check)
        {
            AnsiConsole.MarkupLine("[grey]--check: nothing written.[/]");
            return 0;
        }

        string outPath = settings.Out ?? Path.ChangeExtension(settings.Fnt, ".xfnt");
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
            AnsiConsole.MarkupLine($"[red]error:[/] {ex.Message}");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] wrote [cyan]{outPath}[/]");
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
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
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
}
