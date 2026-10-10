// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Spectre.Console;

namespace CXEX.CLI.Infrastructure;

/// <summary>
/// The CLI's side of running an external tool: an announcement line and the
/// console sinks the library deliberately does not have.
///
/// <para>CXEX.Tools returns a <see cref="ToolResult"/> and writes nothing.
/// Presentation lives here, which is the whole point of the move (DevKit
/// engineering §6, §8): the runner used to call <c>AnsiConsole</c> directly,
/// which made Studio reference the command-line front end in order to launch
/// an emulator.</para>
/// </summary>
public static class CliTools
{
    /// <summary>Console-wired options: stdout in grey, stderr in red.</summary>
    public static ToolRunOptions Options(string? announce = null, string? workingDirectory = null)
    {
        if (announce is { Length: > 0 })
            AnsiConsole.MarkupLine($"[cyan]{Markup.Escape(announce)}[/]");

        return new ToolRunOptions
        {
            WorkingDirectory = workingDirectory,
            OnOutput = line => AnsiConsole.MarkupLine($"[grey]{Markup.Escape(line)}[/]"),
            OnError = line => AnsiConsole.MarkupLine($"[red]{Markup.Escape(line)}[/]"),
        };
    }

    /// <summary>
    /// Reports a result and returns whether it succeeded. A tool that could
    /// not be started, one that timed out and one that ran and failed now read
    /// differently, which they could not when every failure was exit code -1.
    /// </summary>
    public static bool Report(ToolResult result)
    {
        if (result.Ok) return true;
        AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(result.Describe())}");
        if (result.Outcome == ToolOutcome.NotFound)
            AnsiConsole.MarkupLine($"[grey]  looked for:[/] {Markup.Escape(result.ResolvedExecutable)}");
        return false;
    }

    /// <summary>Run a tool by name, announce it, stream it, and report the outcome.</summary>
    public static bool Run(string executable, IEnumerable<string> args,
                           string? announce = null, string? workingDirectory = null)
        => Report(ToolProcess.Run(executable, args, Options(announce, workingDirectory)));
}
