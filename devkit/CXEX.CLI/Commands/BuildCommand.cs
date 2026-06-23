using System.ComponentModel;
using System.IO;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.Build.Parsers;
using CXEX.Build.Engines;
using CXEX.Build.Emitters;
using CXEX.Core.Constants;

namespace CXEX.CLI.Commands;

// FIX: Inherit from Command<BuildCommand.Settings>
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

    // FIX: Override Execute
    public override int Execute(CommandContext context, Settings settings)
    {
        if (!File.Exists(settings.InputPath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] Input file '{settings.InputPath}' not found.");
            return 1;
        }

        // Logic goes here...
        AnsiConsole.MarkupLine($"[green]Building {settings.OutputPath}...[/]");
        return 0;
    }
}