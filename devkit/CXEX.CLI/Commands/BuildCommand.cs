using System.ComponentModel;
using System.IO;
using System.Threading; // ADD THIS
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.Build.Parsers;
using CXEX.Build.Engines;
using CXEX.Build.Emitters;
using CXEX.Core.Constants;

namespace CXEX.CLI.Commands;

public class BuildCommand : Command<BuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<INPUT_ELF>")]
        [Description("Path to the compiled 32-bit ELF file (e.g. kernel.elf)")]
        public string InputPath { get; set; } = string.Empty;

        [CommandArgument(1, "<OUTPUT_CXEX>")]
        [Description("Path to write the CXEX binary (e.g. kernel.xkex)")]
        public string OutputPath { get; set; } = string.Empty;

        [CommandOption("-t|--type")]
        [Description("Executable type: kernel, boot, os, user")]
        [DefaultValue("kernel")]
        public string Type { get; set; } = "kernel";
    }

    // ADD CancellationToken to the signature here:
    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.InputPath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] Input file '{settings.InputPath}' not found.");
            return 1;
        }

        ushort typeCode = settings.Type.ToLower() switch
        {
            "kernel" => CXFlags.TYPE_KERNEL,
            "boot" => CXFlags.TYPE_BOOT,
            "os" => CXFlags.TYPE_OS,
            "user" => CXFlags.TYPE_USER,
            _ => 0
        };

        if (typeCode == 0)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] Invalid type. Must be 'kernel', 'boot', 'os', or 'user'.");
            return 1;
        }

        AnsiConsole.Status()
            .Start($"Compiling {settings.OutputPath}...", ctx =>
            {
                byte[] elfData = File.ReadAllBytes(settings.InputPath);
                var (entryPoint, segments) = ElfParser.Parse(elfData);
                var layout = CXEXLayoutEngine.CreateLayout(entryPoint, segments, typeCode);
                CXEXWriter.WriteExecutable(settings.OutputPath, layout);
            });

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] Compiled [cyan]{settings.OutputPath}[/]");
        return 0;
    }
}