using System;
using System.ComponentModel;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

public class CheckCommand : Command<CheckCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<CMAKE_FILE>")]
        [Description("Path to CMakeLists.txt")]
        public string CmakePath { get; set; } = string.Empty;
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.CmakePath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{settings.CmakePath}' not found.");
            return 1;
        }

        string rootDir = Path.GetDirectoryName(Path.GetFullPath(settings.CmakePath)) ?? string.Empty;
        string cmakeText = File.ReadAllText(settings.CmakePath);

        // Match paths like ${SRC_DIR}/kernel/kmain.c
        var regex = new Regex(@"\$\{SRC_DIR\}/([^\s\)""]+\.(?:c|asm|nasm))");
        var matches = regex.Matches(cmakeText);

        bool missingFiles = false;

        foreach (Match match in matches)
        {
            string relativePath = match.Groups[1].Value;
            string fullPath = Path.Combine(rootDir, relativePath.Replace('/', Path.DirectorySeparatorChar));

            if (!File.Exists(fullPath))
            {
                AnsiConsole.MarkupLine($"[red]MISSING:[/] {relativePath}");
                missingFiles = true;
            }
        }

        if (missingFiles)
        {
            AnsiConsole.MarkupLine("[red]Pre-flight failed: Missing source files referenced in CMakeLists.txt[/]");
            return 1;
        }

        AnsiConsole.MarkupLine("[green]Pre-flight passed:[/] All referenced sources present.");
        return 0;
    }
}