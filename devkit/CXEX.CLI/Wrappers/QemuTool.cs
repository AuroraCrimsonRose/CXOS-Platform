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
}

public static class QemuTool
{
    public static void Run(QemuConfig config)
    {
        AnsiConsole.MarkupLine($"[green]Launching QEMU ({config.MachineType}) with {config.MemoryMb}MB RAM...[/]");

        var args = new StringBuilder();
        args.Append($"-m {config.MemoryMb}M ");
        args.Append($"-machine {config.MachineType} ");

        if (config.EnableAudio)
        {
            args.Append("-machine pcspk-audiodev=speaker ");
            args.Append("-audiodev dsound,id=speaker ");
        }

        if (config.MachineType.ToLower() == "q35")
        {
            // Modern AHCI/SATA setup (Single Unified Disk)
            args.Append($"-drive id=cxkdisk,format=raw,file=\"{config.BootDisk}\",if=none ");
            args.Append("-device ide-hd,drive=cxkdisk,bus=ide.0,bootindex=0 ");
        }
        else
        {
            // Legacy i440FX setup (Multiple IDE disks)
            args.Append($"-drive format=raw,file=\"{config.BootDisk}\",if=ide,index=0 ");

            if (!string.IsNullOrEmpty(config.FsDisk))
                args.Append($"-drive format=raw,file=\"{config.FsDisk}\",if=ide,index=1 ");

            if (!string.IsNullOrEmpty(config.UsbDisk))
                args.Append("-device pci-ohci,id=ohci "); // You would map the USB drive here via QEMU USB syntax
        }

        if (config.EnableNetworking)
        {
            args.Append("-netdev user,id=net0 ");
            args.Append("-device e1000,netdev=net0 ");
        }

        ProcessRunner.Run("qemu-system-i386", args.ToString().Trim());
    }
}