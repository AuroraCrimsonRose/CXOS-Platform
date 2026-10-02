// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;
using System.Diagnostics;
using Spectre.Console;

namespace CXEX.CLI.Infrastructure;

public static class ProcessRunner
{
    public static int Run(string executable, string arguments, string workingDirectory = "")
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = ExecutableResolver.Resolve(executable),
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

/// <summary>
/// Runs a tool purely to find out whether it is there and what version it is.
/// Unlike <see cref="ProcessRunner"/> it prints nothing: a pre-flight probe that
/// echoed every tool's banner would bury the build log before the build started.
/// </summary>
public static class ProcessProbe
{
    /// <summary>
    /// Runs <paramref name="executable"/> and returns its first line of output,
    /// or null if it could not be started at all. A non-zero exit still returns
    /// the output: some tools report their version and exit non-zero.
    /// </summary>
    public static string? FirstLine(string executable, string arguments)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = ExecutableResolver.Resolve(executable),
            Arguments = arguments,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        try
        {
            using var process = Process.Start(startInfo);
            if (process is null) return null;

            string stdout = process.StandardOutput.ReadToEnd();
            string stderr = process.StandardError.ReadToEnd();
            process.WaitForExit();

            string text = string.IsNullOrWhiteSpace(stdout) ? stderr : stdout;
            foreach (var line in text.Split('\n'))
                if (!string.IsNullOrWhiteSpace(line)) return line.Trim();

            return string.Empty;
        }
        catch
        {
            // Win32Exception when the executable is not on PATH - which is the
            // answer the caller wants, not an error to propagate.
            return null;
        }
    }
}
