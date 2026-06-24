using System;
using System.Diagnostics;
using Spectre.Console;

namespace CXEX.CLI.Infrastructure;

public static class ProcessRunner
{
    public static int Run(string executable, string arguments, string workingDirectory = "")
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = executable,
            Arguments = arguments,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = string.IsNullOrEmpty(workingDirectory) ? Environment.CurrentDirectory : workingDirectory
        };

        try
        {
            using var process = new Process { StartInfo = startInfo };

            // Pipe output in real-time to the console
            process.OutputDataReceived += (sender, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data)) AnsiConsole.MarkupLine($"[grey]{Markup.Escape(e.Data)}[/]");
            };

            process.ErrorDataReceived += (sender, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data)) AnsiConsole.MarkupLine($"[red]{Markup.Escape(e.Data)}[/]");
            };

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();

            return process.ExitCode;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Fatal Error launching '{executable}':[/] {ex.Message}");
            return -1;
        }
    }
}