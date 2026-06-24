using System.IO;
using System.Text;
using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

public class BochsConfig
{
    public int MemoryMb { get; set; } = 2048;
    public string BiosRomPath { get; set; } = @"C:\Program Files\Bochs-3.0\BIOS-bochs-latest";
    public string VgaRomPath { get; set; } = @"C:\Program Files\Bochs-3.0\VGABIOS-lgpl-latest.bin";
    public string BootDisk { get; set; } = string.Empty;
    public string? FsDisk { get; set; }
    public string? UsbDisk { get; set; }
}

public static class BochsTool
{
    public static void Run(BochsConfig config, string workingDirectory)
    {
        string configPath = Path.Combine(workingDirectory, "bochsrc_generated.txt");
        AnsiConsole.MarkupLine($"[cyan]Generating dynamic Bochs config -> {configPath}[/]");

        var sb = new StringBuilder();
        sb.AppendLine($"megs: {config.MemoryMb}");
        sb.AppendLine("cpu: ips=2100000000");
        sb.AppendLine("clock: sync=realtime, time0=local");
        sb.AppendLine($"romimage: file=\"{config.BiosRomPath}\"");
        sb.AppendLine($"vgaromimage: file=\"{config.VgaRomPath}\"");
        sb.AppendLine("boot: disk");

        sb.AppendLine($"ata0-master: type=disk, path=\"{config.BootDisk}\", mode=flat");

        if (!string.IsNullOrEmpty(config.FsDisk))
            sb.AppendLine($"ata0-slave: type=disk, path=\"{config.FsDisk}\", mode=flat");

        sb.AppendLine("log: bochs.log");
        sb.AppendLine("panic: action=report");
        sb.AppendLine("error: action=report");
        sb.AppendLine("info: action=report");

        if (!string.IsNullOrEmpty(config.UsbDisk))
            sb.AppendLine($"usb_ohci: enabled=1, port1=disk, options1=\"path:{config.UsbDisk}\"");

        sb.AppendLine("ne2k: enabled=1, mac=b0:c4:20:00:00:01, ethmod=slirp");

        File.WriteAllText(configPath, sb.ToString());

        AnsiConsole.MarkupLine("[green]Launching Bochs...[/]");

        // Launch Bochs using the generated file
        ProcessRunner.Run("bochs", $"-f \"{configPath}\" -q", workingDirectory);
    }
}