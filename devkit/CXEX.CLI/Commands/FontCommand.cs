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
/// `cxk font build` - a published BDF font -> a console XFNT for one standard
/// character set.
///
/// CXOS uses existing fonts rather than drawing its own (decided 2026-10-16);
/// BDF is the source format they are published in. The conversion is strict:
/// a font that lacks a character of the set fails with the missing codepoints
/// named, and a glyph that does not fit the cell is refused rather than
/// cropped.
/// </summary>
public class FontBuildCommand : Command<FontBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<BDF>")]
        [Description("The font's BDF source, as its publisher ships it")]
        public string Bdf { get; set; } = string.Empty;

        [CommandOption("-o|--out")]
        [Description("Output .xfnt. Default: the source's name with .xfnt")]
        public string? Out { get; set; }

        [CommandOption("--charset")]
        [Description("The standard character set the console's bytes mean: cp437 (default)")]
        [DefaultValue("cp437")]
        public string Charset { get; set; } = "cp437";

        [CommandOption("--face")]
        [Description("The face name stored in the font, at most 8 characters. Never the source font's own name: OFL fonts reserve it for unmodified copies")]
        [DefaultValue("Console")]
        public string Face { get; set; } = "Console";

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
        if (!Enum.TryParse<ConsoleFont.Charset>(settings.Charset, true, out var set))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --charset must be cp437; got '{Markup.Escape(settings.Charset)}'");
            return 1;
        }

        ConsoleFont.Result result;
        try
        {
            var font = BdfFont.Load(settings.Bdf);
            result = ConsoleFont.Build(font, set, settings.Face);
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
