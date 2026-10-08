// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;
using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

public class QemuConfig
{
    public int MemoryMb { get; set; } = 4096; // 4G default
    public string MachineType { get; set; } = "q35"; // "q35" or "pc" (i440FX)
    public string BootDisk { get; set; } = string.Empty;
    public string? FsDisk { get; set; }
    public string? UsbDisk { get; set; }
    public bool EnableAudio { get; set; } = true;
    public bool EnableNetworking { get; set; } = true;

    /// <summary>Write a pcap of the NIC's traffic here.</summary>
    public string? NetTracePath { get; set; }

    /// <summary>Throw away everything the guest writes to its disks.</summary>
    public bool Snapshot { get; set; }

    /// <summary>"stdio", "none", or a file path for COM1.</summary>
    public string? Serial { get; set; }

    /// <summary>No emulator window.</summary>
    public bool Headless { get; set; }

    /// <summary>Value of -icount shift=N, or null to leave timing alone.</summary>
    public string? ICountShift { get; set; }
}

public static class QemuTool
{
    /// <summary>
    /// Builds the argument string. Separate from Run so `cxk run --dry-run`
    /// can print exactly what would be executed, which also makes the option
    /// wiring testable without starting an emulator.
    /// </summary>
    public static string BuildArguments(QemuConfig config)
    {
        var args = new StringBuilder();
        args.Append($"-m {config.MemoryMb}M ");
        args.Append($"-machine {config.MachineType} ");

        if (config.EnableAudio)
        {
            args.Append("-machine pcspk-audiodev=speaker ");
            args.Append("-audiodev dsound,id=speaker ");
        }

        if (config.MachineType.Equals("q35", StringComparison.OrdinalIgnoreCase))
        {
            // Modern AHCI/SATA setup (single unified disk)
            args.Append($"-drive id=cxkdisk,format=raw,file=\"{config.BootDisk}\",if=none ");
            args.Append("-device ide-hd,drive=cxkdisk,bus=ide.0,bootindex=0 ");
        }
        else
        {
            // Legacy i440FX setup (multiple IDE disks)
            args.Append($"-drive format=raw,file=\"{config.BootDisk}\",if=ide,index=0 ");

            if (!string.IsNullOrEmpty(config.FsDisk))
                args.Append($"-drive format=raw,file=\"{config.FsDisk}\",if=ide,index=1 ");

            if (!string.IsNullOrEmpty(config.UsbDisk))
                args.Append("-device pci-ohci,id=ohci ");
        }

        // Applies to every drive, so it follows the -drive options.
        if (config.Snapshot)
            args.Append("-snapshot ");

        if (config.EnableNetworking)
        {
            args.Append("-netdev user,id=net0 ");
            args.Append("-device e1000,netdev=net0 ");

            if (!string.IsNullOrEmpty(config.NetTracePath))
                args.Append($"-object filter-dump,id=dump0,netdev=net0,file=\"{config.NetTracePath}\" ");
        }

        if (!string.IsNullOrEmpty(config.Serial))
        {
            // "stdio" and "none" are QEMU chardevs as written; anything else is
            // taken as a path, which is the common case (-serial file:boot.log).
            string serial = config.Serial is "stdio" or "none"
                ? config.Serial
                : $"file:{config.Serial}";
            args.Append($"-serial {serial} ");
        }

        if (config.Headless)
            args.Append("-display none ");

        if (!string.IsNullOrEmpty(config.ICountShift))
            args.Append($"-icount shift={config.ICountShift} ");

        return args.ToString().Trim();
    }

    public static int Run(QemuConfig config)
    {
        AnsiConsole.MarkupLine($"[green]Launching QEMU ({config.MachineType}) with {config.MemoryMb}MB RAM...[/]");
        return ProcessRunner.Run("qemu-system-i386", BuildArguments(config));
    }
}
