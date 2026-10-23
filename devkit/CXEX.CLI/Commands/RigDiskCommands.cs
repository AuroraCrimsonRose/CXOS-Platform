// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Disk;
using CXEX.Disk.Parsers;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// What the rig commands accept as a disk: an image file, or a whole disk
/// (<c>disk5</c>, <c>\\.\PhysicalDrive5</c>, <c>/dev/sdb</c>), always read-only.
/// </summary>
internal static class RigDisk
{
    public const string Help = "An image file, or a whole disk: disk5 (the number from cxk rig disks), \\\\.\\PhysicalDrive5, /dev/sdb. Read-only";

    /// <summary>The disk opened for reading, or null with the reason already printed.</summary>
    public static Stream? Open(string arg)
    {
        string? device = RawDisk.DevicePath(arg);
        try
        {
            if (device is not null) return RawDisk.OpenRead(device);
            if (!File.Exists(arg))
            {
                AnsiConsole.MarkupLine($"[red]error:[/] not found: '{Markup.Escape(arg)}' (a disk is named disk5; see cxk rig disks)");
                return null;
            }
            return new FileStream(arg, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        }
        catch (UnauthorizedAccessException)
        {
            AnsiConsole.MarkupLine(OperatingSystem.IsWindows()
                ? $"[red]error:[/] reading {Markup.Escape(device ?? arg)} needs an elevated prompt (Run as administrator)"
                : $"[red]error:[/] reading {Markup.Escape(device ?? arg)} needs root, or membership of the disk group");
            return null;
        }
        catch (Exception ex) when (ex is IOException or ArgumentException)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] cannot open '{Markup.Escape(device ?? arg)}': {Markup.Escape(ex.Message)}");
            return null;
        }
    }
}

/// <summary>Lists the machine's disks, to find the stick's number. Needs no elevation.</summary>
public class RigDisksCommand : Command<RigDisksCommand.Settings>
{
    public class Settings : CommandSettings { }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!OperatingSystem.IsWindows())
        {
            AnsiConsole.MarkupLine("[grey]listing disks is Windows-only; use lsblk, then pass /dev/sdX[/]");
            return 0;
        }
        var disks = RawDisk.List();
        if (disks.Count == 0)
        {
            AnsiConsole.MarkupLine("[yellow]no disks found[/]");
            return 1;
        }
        var t = new Table().Border(TableBorder.Simple)
            .AddColumn("Name").AddColumn(new TableColumn("Size").RightAligned()).AddColumn("Bus").AddColumn("Removable").AddColumn("Model");
        foreach (var d in disks)
            t.AddRow($"disk{d.Number}", Size(d.Bytes), d.Bus, d.Removable ? "yes" : "", Markup.Escape(d.Model));
        AnsiConsole.Write(t);
        return 0;
    }

    private static string Size(long b) => b <= 0 ? "?" : b >= 1L << 30 ? $"{b / (double)(1L << 30):0.0} GiB" : $"{b >> 20} MiB";
}

/// <summary>
/// Copies a disk's CXOS part - up to the end of its XBPT table's disk, or of
/// its last partition - into an image file, so every image tool can read what
/// the rig left on the stick.
/// </summary>
public class RigDumpCommand : Command<RigDumpCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<DISK>")]
        [Description(RigDisk.Help)]
        public string DiskPath { get; set; } = string.Empty;

        [CommandArgument(1, "<OUTPUT>")]
        [Description("The image file to write. Must not be a disk")]
        public string OutputPath { get; set; } = string.Empty;

        [CommandOption("--sectors")]
        [Description("Copy this many sectors instead of the extent the XBPT table gives")]
        public long? Sectors { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (RawDisk.DevicePath(settings.OutputPath) is not null)
        {
            AnsiConsole.MarkupLine("[red]error:[/] the output is a disk; rig dump only writes image files");
            return 1;
        }
        using var disk = RigDisk.Open(settings.DiskPath);
        if (disk is null) return 1;

        long sectors;
        if (settings.Sectors is long s)
        {
            if (s <= 0) { AnsiConsole.MarkupLine("[red]error:[/] --sectors must be positive"); return 1; }
            sectors = s;
        }
        else
        {
            var lba1 = new byte[RawDisk.Sector];
            disk.Seek(RawDisk.Sector, SeekOrigin.Begin);
            if (disk.ReadAtLeast(lba1, lba1.Length, throwOnEndOfStream: false) < lba1.Length
                || !lba1.AsSpan(0, 4).SequenceEqual(XbptParser.Magic))
            {
                AnsiConsole.MarkupLine("[red]error:[/] no XBPT table at sector 1 - not a CXOS disk; pass --sectors to copy anyway");
                return 1;
            }
            var layout = XbptParser.Parse(lba1, 0);
            sectors = layout.DiskSizeBytes / RawDisk.Sector;
            foreach (var p in layout.Partitions) sectors = Math.Max(sectors, p.StartLba + p.SectorCount);
        }
        long bytes = sectors * RawDisk.Sector;
        if (bytes > disk.Length)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {sectors} sectors is past the end of the disk ({disk.Length / RawDisk.Sector} sectors)");
            return 1;
        }

        disk.Seek(0, SeekOrigin.Begin);
        using (var outFs = new FileStream(settings.OutputPath, FileMode.Create, FileAccess.Write))
        {
            var buf = new byte[1 << 20];
            long left = bytes;
            while (left > 0)
            {
                int n = disk.Read(buf, 0, (int)Math.Min(buf.Length, left));
                if (n <= 0)
                {
                    AnsiConsole.MarkupLine($"[red]error:[/] the disk ended {left} bytes short");
                    return 1;
                }
                outFs.Write(buf, 0, n);
                left -= n;
            }
        }
        AnsiConsole.MarkupLine($"[green]copied[/] {sectors} sectors ({bytes >> 20} MiB) to {Markup.Escape(settings.OutputPath)}");
        return 0;
    }
}
