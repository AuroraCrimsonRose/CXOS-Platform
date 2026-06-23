using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.Build.Engines;
using CXEX.Build.Emitters;

namespace CXEX.CLI.Commands;

public class ImageCommand : Command<ImageCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandOption("--out <PATH>")]
        [Description("Path to the output disk image")]
        public string OutputPath { get; set; } = string.Empty;

        [CommandOption("--size-mb <SIZE>")]
        [DefaultValue(64)]
        public int SizeMb { get; set; }

        [CommandOption("--stage1 <PATH>")]
        public string? Stage1 { get; set; }

        [CommandOption("--stage2 <PATH>")]
        public string? Stage2 { get; set; }

        [CommandOption("--kernel <PATH>")]
        public string? Kernel { get; set; }

        [CommandOption("--boot-mb <SIZE>")]
        [DefaultValue(8)]
        public int BootMb { get; set; }

        [CommandOption("--stage <NAME=PATH>")]
        public string[]? StageFiles { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(settings.OutputPath))
        {
            AnsiConsole.MarkupLine("[red]Error:[/] --out path is required.");
            return 1;
        }

        AnsiConsole.Status()
            .Start($"Compiling Disk Image {settings.OutputPath}...", ctx =>
            {
                // 1. Read Boot Binaries
                byte[]? stage1 = string.IsNullOrEmpty(settings.Stage1) ? null : File.ReadAllBytes(settings.Stage1);
                byte[]? stage2 = string.IsNullOrEmpty(settings.Stage2) ? null : File.ReadAllBytes(settings.Stage2);
                byte[]? kernel = string.IsNullOrEmpty(settings.Kernel) ? null : File.ReadAllBytes(settings.Kernel);

                // 2. Read Stage Payload Files (XSTG)
                var stagedFiles = new List<StagedFile>();
                int stagedBytes = 0;

                if (settings.StageFiles != null)
                {
                    foreach (var spec in settings.StageFiles)
                    {
                        var parts = spec.Split('=');
                        if (parts.Length != 2)
                        {
                            AnsiConsole.MarkupLine($"[yellow]Warning:[/] Ignoring malformed stage file '{spec}'. Expected NAME=PATH.");
                            continue;
                        }

                        var fileData = File.ReadAllBytes(parts[1]);
                        stagedFiles.Add(new StagedFile { Name = parts[0], Data = fileData });
                        stagedBytes += fileData.Length;
                    }
                }

                int stage2Bytes = stage2?.Length ?? 0;

                // 3. Math Out The Geometry (aligning partitions to 1MB)
                var map = DiskGeometryEngine.CalculateLayout(settings.SizeMb, settings.BootMb, stage2Bytes, stagedBytes);

                // 4. Emit The Final Image
                XBPTImageWriter.WriteImage(settings.OutputPath, map, stage1, stage2, kernel, stagedFiles);
            });

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] Wrote bootable image to [cyan]{settings.OutputPath}[/] ({settings.SizeMb} MB)");
        return 0;
    }
}