using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.Core.Utilities;

namespace CXEX.CLI.Commands;

public class RawImageCommand : Command<RawImageCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandOption("--out <PATH>")]
        [Description("Path to the output disk image")]
        public string OutputPath { get; set; } = string.Empty;

        [CommandOption("--size-mb <SIZE>")]
        [Description("Total size of the raw disk in MB")]
        [DefaultValue(4)]
        public int SizeMb { get; set; }

        [CommandOption("--boot <PATH>")]
        public string? Boot { get; set; }

        [CommandOption("--stage2 <PATH>")]
        public string? Stage2 { get; set; }

        [CommandOption("--kernel <PATH>")]
        public string? Kernel { get; set; }

        [CommandOption("--kernel-lba <LBA>")]
        [Description("LBA sector offset to write the kernel")]
        [DefaultValue(33)] // pad_boot.ps1 uses 33, pad_disk.ps1 uses 18
        public int KernelLba { get; set; }

        [CommandOption("--patch-ksnt")]
        [Description("Scan Stage2 and patch the KSNT sector count marker")]
        [DefaultValue(false)]
        public bool PatchKsnt { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(settings.OutputPath))
        {
            AnsiConsole.MarkupLine("[red]Error:[/] --out path is required.");
            return 1;
        }

        long diskSize = settings.SizeMb * 1024L * 1024L;
        byte[] disk = new byte[diskSize]; // Automatically zero-filled (replaces make_fs_img.ps1)

        try
        {
            if (!string.IsNullOrEmpty(settings.Boot))
            {
                byte[] boot = File.ReadAllBytes(settings.Boot);
                Array.Copy(boot, 0, disk, 0, boot.Length);
            }

            if (!string.IsNullOrEmpty(settings.Stage2))
            {
                byte[] stage2 = File.ReadAllBytes(settings.Stage2);
                Array.Copy(stage2, 0, disk, 512, stage2.Length);
            }

            if (!string.IsNullOrEmpty(settings.Kernel))
            {
                byte[] kernel = File.ReadAllBytes(settings.Kernel);
                long kOffset = settings.KernelLba * 512L;

                if (kOffset + kernel.Length > diskSize)
                {
                    AnsiConsole.MarkupLine("[red]Error:[/] Kernel is too large for the specified disk size.");
                    return 1;
                }

                Array.Copy(kernel, 0, disk, kOffset, kernel.Length);

                // Replaces the byte-level patching from pad_disk.ps1
                if (settings.PatchKsnt)
                {
                    int kernelSectors = (int)Math.Ceiling((double)kernel.Length / 512);
                    bool patched = false;
                    for (int i = 512; i < 512 + (16 * 512) - 4; i++)
                    {
                        if (disk[i] == 0x54 && disk[i + 1] == 0x4E && disk[i + 2] == 0x53 && disk[i + 3] == 0x4B)
                        {
                            MemoryPrimitives.WriteU32(disk.AsSpan(), i + 4, (uint)kernelSectors);
                            patched = true;
                            break;
                        }
                    }
                    if (!patched) AnsiConsole.MarkupLine("[yellow]Warning:[/] KSNT marker not found in Stage2.");
                }
            }

            string? dir = Path.GetDirectoryName(settings.OutputPath);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

            File.WriteAllBytes(settings.OutputPath, disk);
            AnsiConsole.MarkupLine($"[green]SUCCESS:[/] Raw image written to [cyan]{settings.OutputPath}[/]");
            return 0;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error generating raw image:[/] {ex.Message}");
            return 1;
        }
    }
}