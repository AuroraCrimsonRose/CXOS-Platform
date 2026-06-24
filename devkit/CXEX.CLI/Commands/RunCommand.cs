using CXEX.CLI.Wrappers;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Diagnostics;
using System.Threading;

namespace CXEX.CLI.Commands;

public class RunCommand : Command<RunCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE_PATH>")]
        [Description("Path to the bootable CXK disk image")]
        public string ImagePath { get; set; } = string.Empty;

        [CommandOption("-e|--emu")]
        [Description("Emulator to use (qemu or bochs)")]
        [DefaultValue("qemu")]
        public string Emulator { get; set; } = "qemu";
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {

        if (settings.Emulator.ToLower() == "qemu")
        {
            var qemuConfig = new QemuConfig
            {
                BootDisk = settings.ImagePath, // e.g., cxk_disk.img
                MachineType = "q35",           // Map this to a CLI flag if you want!
                MemoryMb = 4096
            };
            QemuTool.Run(qemuConfig);
            return 0;
        }
        else if (settings.Emulator.ToLower() == "bochs")
        {
            var bochsConfig = new BochsConfig
            {
                BootDisk = settings.ImagePath,
                MemoryMb = 2048
            };

            // We pass the directory so the bochsrc.txt is generated right next to the image
            string imgDir = System.IO.Path.GetDirectoryName(settings.ImagePath) ?? Environment.CurrentDirectory;
            BochsTool.Run(bochsConfig, imgDir);
            return 0;
        }

        if (!System.IO.File.Exists(settings.ImagePath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] Disk image not found at '{settings.ImagePath}'");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]Launching {settings.Emulator.ToUpper()}...[/]");

        var startInfo = new ProcessStartInfo
        {
            UseShellExecute = false
        };

        if (settings.Emulator.ToLower() == "qemu")
        {
            // Mirrors your run_qemu.bat logic
            startInfo.FileName = "qemu-system-i386";
            startInfo.Arguments = $"-m 256M -drive file=\"{settings.ImagePath}\",format=raw,index=0,media=disk -serial stdio -rtc base=localtime";
        }
        else if (settings.Emulator.ToLower() == "bochs")
        {
            // Requires a valid bochsrc.txt in the working directory
            startInfo.FileName = "bochs";
            startInfo.Arguments = "-f bochsrc2.txt -q";
        }
        else
        {
            AnsiConsole.MarkupLine("[red]Error:[/] Unsupported emulator. Use 'qemu' or 'bochs'.");
            return 1;
        }

        try
        {
            using var process = Process.Start(startInfo);
            process?.WaitForExit();
            return process?.ExitCode ?? 0;
        }
        catch (System.Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Failed to launch emulator:[/] {ex.Message}");
            AnsiConsole.MarkupLine("[yellow]Tip:[/] Ensure the emulator is installed and added to your system PATH.");
            return 1;
        }
    }
}