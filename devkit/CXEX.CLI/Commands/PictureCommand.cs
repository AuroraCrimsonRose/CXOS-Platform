// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading;
using CXEX.Build.Pictures;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// `cxk picture build` - a baseline JPEG -> QOI, scaled down to cover a
/// screen. The OS reads QOI and nothing heavier (std/picture.xfxn), so the
/// desktop's background is converted here. A source still carrying metadata
/// (EXIF, XMP, comments: where a camera writes its GPS position) is refused:
/// strip it first, so what is checked in carries none.
/// </summary>
public class PictureBuildCommand : Command<PictureBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<JPEG>")]
        [Description("The picture, a baseline JPEG without metadata")]
        public string Jpeg { get; set; } = string.Empty;

        [CommandOption("-o|--out")]
        [Description("Output .qoi. Default: the source's name with .qoi")]
        public string? Out { get; set; }

        [CommandOption("--cover")]
        [Description("WxH: scaled down (never up), aspect kept, until it just covers a screen this size. Default 1920x1080")]
        [DefaultValue("1920x1080")]
        public string Cover { get; set; } = "1920x1080";
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        try
        {
            var parts = settings.Cover.Split('x');
            if (parts.Length != 2 || !int.TryParse(parts[0], out int cw) || !int.TryParse(parts[1], out int ch) || cw < 1 || ch < 1 || cw > 16384 || ch > 16384)
                throw new ArgumentException($"--cover must be WxH, got '{settings.Cover}'");
            byte[] data = File.ReadAllBytes(settings.Jpeg);
            var meta = Jpeg.Segments(data).Where(s => Jpeg.IsMetadata(s.Marker)).Select(s => s.Name).ToList();
            if (meta.Count > 0)
                throw new InvalidDataException($"it carries metadata ({string.Join(", ", meta)}): run `cxk picture strip` on it first");
            var pic = Jpeg.Decode(data);
            var (w, h) = PictureScale.CoverSize(pic.Width, pic.Height, cw, ch);
            var small = PictureScale.Resize(pic, w, h);
            byte[] qoi = Qoi.Encode(small);
            string outPath = settings.Out ?? Path.ChangeExtension(settings.Jpeg, ".qoi");
            File.WriteAllBytes(outPath, qoi);
            // Read back through the checking reader: the build must not make a file the OS refuses.
            var back = Qoi.Decode(File.ReadAllBytes(outPath));
            if (back.Width != w || back.Height != h || !back.Rgb.AsSpan().SequenceEqual(small.Rgb))
                throw new InvalidDataException("the picture read back differently");
            AnsiConsole.MarkupLine($"[green]SUCCESS:[/] {pic.Width}x{pic.Height} -> {w}x{h}, {qoi.Length} bytes: [cyan]{Markup.Escape(outPath)}[/]");
            return 0;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(settings.Jpeg)}: {Markup.Escape(ex.Message)}");
            return 1;
        }
    }
}

/// <summary>
/// `cxk picture strip` - removes a JPEG's metadata in place (EXIF with its
/// GPS position, XMP, IPTC, comments), copying every other byte, so the
/// pixels are exactly the original's.
/// </summary>
public class PictureStripCommand : Command<PictureStripCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<JPEG>")]
        [Description("The pictures to strip, in place")]
        public string[] Files { get; set; } = Array.Empty<string>();

        [CommandOption("--check")]
        [Description("Name what would go; write nothing. Exits 1 if any file carries metadata")]
        public bool Check { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        int rc = 0;
        foreach (var f in settings.Files)
        {
            try
            {
                byte[] data = File.ReadAllBytes(f);
                byte[] stripped = Jpeg.Strip(data, out var dropped);
                if (dropped.Count == 0) { AnsiConsole.MarkupLine($"clean  {Markup.Escape(f)}"); continue; }
                string what = string.Join(", ", dropped);
                if (settings.Check) { AnsiConsole.MarkupLine($"[yellow]metadata[/]  {Markup.Escape(f)}: {what}"); rc = 1; continue; }
                File.WriteAllBytes(f, stripped);
                AnsiConsole.MarkupLine($"[green]stripped[/]  {Markup.Escape(f)}: {what}, {data.Length - stripped.Length} bytes");
            }
            catch (Exception ex)
            {
                AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(f)}: {Markup.Escape(ex.Message)}");
                rc = 1;
            }
        }
        return rc;
    }
}
