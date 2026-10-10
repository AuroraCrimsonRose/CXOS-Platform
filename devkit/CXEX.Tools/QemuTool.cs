// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)

namespace CXEX.Tools;

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
    /// Builds the argument list. Separate from Run so `cxk run --dry-run` can
    /// print exactly what would be executed, which also makes the option
    /// wiring testable without starting an emulator.
    ///
    /// <para>A list, not a string: a disk image path containing a space used
    /// to depend on this method remembering to quote it, and the quoting was
    /// its own escaping convention rather than the runtime's
    /// (DevKit security review §9).</para>
    /// </summary>
    public static List<string> BuildArguments(QemuConfig config)
    {
        var args = new List<string>
        {
            "-m", $"{config.MemoryMb}M",
            "-machine", config.MachineType,
        };

        if (config.EnableAudio)
        {
            args.Add("-machine"); args.Add("pcspk-audiodev=speaker");
            args.Add("-audiodev"); args.Add("dsound,id=speaker");
        }

        if (config.MachineType.Equals("q35", StringComparison.OrdinalIgnoreCase))
        {
            // Modern AHCI/SATA setup (single unified disk)
            args.Add("-drive"); args.Add($"id=cxkdisk,format=raw,file={config.BootDisk},if=none");
            args.Add("-device"); args.Add("ide-hd,drive=cxkdisk,bus=ide.0,bootindex=0");
        }
        else
        {
            // Legacy i440FX setup (multiple IDE disks)
            args.Add("-drive"); args.Add($"format=raw,file={config.BootDisk},if=ide,index=0");

            if (!string.IsNullOrEmpty(config.FsDisk))
            {
                args.Add("-drive"); args.Add($"format=raw,file={config.FsDisk},if=ide,index=1");
            }

            if (!string.IsNullOrEmpty(config.UsbDisk))
            {
                args.Add("-device"); args.Add("pci-ohci,id=ohci");
            }
        }

        // Applies to every drive, so it follows the -drive options.
        if (config.Snapshot) args.Add("-snapshot");

        if (config.EnableNetworking)
        {
            args.Add("-netdev"); args.Add("user,id=net0");
            args.Add("-device"); args.Add("e1000,netdev=net0");

            if (!string.IsNullOrEmpty(config.NetTracePath))
            {
                args.Add("-object");
                args.Add($"filter-dump,id=dump0,netdev=net0,file={config.NetTracePath}");
            }
        }

        if (!string.IsNullOrEmpty(config.Serial))
        {
            // "stdio" and "none" are QEMU chardevs as written; anything else is
            // taken as a path, which is the common case (-serial file:boot.log).
            args.Add("-serial");
            args.Add(config.Serial is "stdio" or "none" ? config.Serial : $"file:{config.Serial}");
        }

        if (config.Headless) { args.Add("-display"); args.Add("none"); }

        if (!string.IsNullOrEmpty(config.ICountShift))
        {
            args.Add("-icount"); args.Add($"shift={config.ICountShift}");
        }

        return args;
    }

    /// <summary>
    /// The command line as it would be typed, for `--dry-run` and logs.
    /// </summary>
    public static string CommandLine(QemuConfig config) =>
        ToolProcess.Quote(Executable, BuildArguments(config));

    public const string Executable = "qemu-system-i386";

    /// <summary>
    /// Boots the image. No timeout by default: an emulator session lasts as
    /// long as the person watching it wants, which is the one case where the
    /// runner's 30-minute budget would be wrong.
    /// </summary>
    public static ToolResult Run(QemuConfig config, ToolRunOptions? options = null)
    {
        options ??= new ToolRunOptions { Timeout = TimeSpan.Zero };
        return ToolProcess.Run(Executable, BuildArguments(config), options);
    }
}
