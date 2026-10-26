// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using CXEX.Lang.Abi;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Pre-flight: confirm the kernel's ABI header and the X prelude are what
/// <c>abi/kernel.xfil</c> generates (roadmap step 6a1), then that the two agree.
///
/// <para>The prelude's constants and structures are generated from the copy of
/// <c>abi/cxk_abi.h</c> embedded in this <c>cxk</c> when it was built
/// (<see cref="CXEX.Lang.Abi.AbiModel"/>). So what this catches now is a <c>cxk</c> older than
/// the header on disk. That is easy to have, because <c>tools/cxk.exe</c> is a gitignored
/// prebuilt. The comparison includes struct field types. It used to guard a hand-kept copy in
/// another repository, which drifted once and stopped the userland compiling.</para>
///
/// <para>Run this before building CXK's userland. Exit code 0 means the mirrored surface
/// agrees; 1 means it does not. The comparison itself lives in
/// <see cref="CXEX.Lang.Abi.AbiSync"/> so the tests and any other front end can share it.</para>
/// </summary>
public class CheckAbiCommand : Command<CheckAbiCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "[HEADER]")]
        [Description("Path to cxk_abi.h. Default: auto-detected from a sibling CXK checkout.")]
        public string? HeaderPath { get; set; }

        [CommandOption("--strict")]
        [Description("Treat warnings (prelude entries absent from the header) as failures too.")]
        public bool Strict { get; set; }

        [CommandOption("--quiet")]
        [Description("Only print failures and the summary line.")]
        public bool Quiet { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        string? headerPath = settings.HeaderPath is { Length: > 0 }
            ? Path.GetFullPath(settings.HeaderPath)
            : LocateHeader();

        if (headerPath is null)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] could not find cxk_abi.h.");
            AnsiConsole.MarkupLine("[grey]  Pass it explicitly:  cxk check-abi path/to/abi/cxk_abi.h[/]");
            AnsiConsole.MarkupLine("[grey]  or set CXK_ROOT to the CXK repository root.[/]");
            return 1;
        }

        if (!File.Exists(headerPath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{Markup.Escape(headerPath)}' not found.");
            return 1;
        }

        AnsiConsole.MarkupLine($"[grey]header:[/] {Markup.Escape(headerPath)}");

        // The header is generated from kernel.xfil beside it (roadmap step 6a1), and
        // the prelude from the copy of kernel.xfil built into this cxk. Either can
        // fall behind the file on disk, and each has its own fix.
        int generated = 0;
        string xfilPath = Path.Combine(Path.GetDirectoryName(headerPath)!, "kernel.xfil");
        if (!File.Exists(xfilPath))
        {
            AnsiConsole.MarkupLine($"[red]DRIFT:[/] {Markup.Escape(xfilPath)} not found; the header is generated from it.");
            generated++;
        }
        else
        {
            string xfilText = File.ReadAllText(xfilPath).Replace("\r\n", "\n");
            if (xfilText != AbiModel.EmbeddedXfil.Replace("\r\n", "\n"))
            {
                AnsiConsole.MarkupLine("[red]DRIFT:[/] this cxk was built from a different abi/kernel.xfil. " +
                                       "Rebuild it (and refresh tools/cxk.exe), then re-run.");
                generated++;
            }
            try
            {
                // The header as given, the rest under the repository it sits in.
                string root = Path.GetDirectoryName(Path.GetDirectoryName(headerPath)!)!;
                foreach ((string rel, string expected) in XfilEmit.Outputs(XfilParser.Parse(xfilText)))
                {
                    string path = rel == XfilEmit.KernelHeader ? headerPath : Path.Combine(root, rel);
                    if (File.Exists(path) && File.ReadAllText(path).Replace("\r\n", "\n") == expected) continue;
                    AnsiConsole.MarkupLine($"[red]DRIFT:[/] {Markup.Escape(rel)} is not what abi/kernel.xfil generates. " +
                                           "Run `cxk abi generate`; never edit a generated file by hand.");
                    generated++;
                }
            }
            catch (XfilException e)
            {
                AnsiConsole.MarkupLine($"[red]DRIFT:[/] abi/kernel.xfil, {Markup.Escape(e.Message)}");
                generated++;
            }
        }

        AbiSyncReport report = AbiSync.Compare(File.ReadAllText(headerPath), AbiPrelude.Generate());

        foreach (AbiFinding f in report.Findings)
        {
            switch (f.Kind)
            {
                case AbiFindingKind.Error:
                    AnsiConsole.MarkupLine($"[red]DRIFT:[/] {Markup.Escape(f.Message)}");
                    break;
                case AbiFindingKind.Warning:
                    AnsiConsole.MarkupLine($"[yellow]warning:[/] {Markup.Escape(f.Message)}");
                    break;
                default:
                    if (!settings.Quiet)
                        AnsiConsole.MarkupLine($"[grey]note:[/] {Markup.Escape(f.Message)}");
                    break;
            }
        }

        if (generated > 0)
        {
            AnsiConsole.MarkupLine($"[red]ABI check failed:[/] {generated} generated file(s) out of step with abi/kernel.xfil.");
            return 1;
        }

        int errors = report.Count(AbiFindingKind.Error);
        int warnings = report.Count(AbiFindingKind.Warning);

        if (!settings.Quiet)
        {
            AnsiConsole.MarkupLine(
                $"[grey]compared:[/] {report.HeaderConstants} header constants / {report.HeaderStructs} structs " +
                $"against {report.PreludeConstants} prelude constants / {report.PreludeStructs} structs");
        }

        if (errors > 0)
        {
            AnsiConsole.MarkupLine(
                $"[red]ABI check failed:[/] {errors} difference(s) between cxk_abi.h and the X prelude.");
            AnsiConsole.MarkupLine(
                "[grey]  The prelude is generated from the header this cxk was built with, so this cxk is " +
                "older than abi/cxk_abi.h. Rebuild it (and refresh tools/cxk.exe), then re-run. If a " +
                "rebuilt cxk still fails, the header uses something CXEX.Lang/Abi/AbiModel.cs cannot " +
                "express yet.[/]");
            return 1;
        }

        if (settings.Strict && warnings > 0)
        {
            AnsiConsole.MarkupLine($"[red]ABI check failed (--strict):[/] {warnings} warning(s).");
            return 1;
        }

        AnsiConsole.MarkupLine(
            warnings > 0
                ? $"[green]ABI check passed[/] [grey]({warnings} warning(s))[/]"
                : "[green]ABI check passed:[/] cxk_abi.h and the prelude are what abi/kernel.xfil generates.");
        return 0;
    }

    /// <summary>
    /// Finds cxk_abi.h without being told. In order: <c>CXK_ROOT</c>, then a sibling CXK
    /// checkout walking up from the working directory. The header sits at
    /// <c>&lt;repo&gt;/abi/cxk_abi.h</c> - the repository root contains a nested CXK
    /// directory - so both layouts are tried at each level.
    /// </summary>
    internal static string? LocateHeader()
    {
        string? envRoot = Environment.GetEnvironmentVariable("CXK_ROOT");
        if (!string.IsNullOrEmpty(envRoot))
        {
            string? hit = ProbeRoot(envRoot);
            if (hit is not null) return hit;
        }

        string dir = Directory.GetCurrentDirectory();
        for (int depth = 0; depth < 6 && dir.Length > 0; depth++)
        {
            string? hit = ProbeRoot(dir)
                       ?? ProbeRoot(Path.Combine(dir, "CXK"))
                       ?? ProbeRoot(Path.Combine(dir, "..", "CXK"));
            if (hit is not null) return hit;

            string? parent = Path.GetDirectoryName(dir);
            if (parent is null || parent == dir) break;
            dir = parent;
        }
        return null;
    }

    /// <summary>Returns the header under <paramref name="root"/>, trying the nested and flat layouts.</summary>
    private static string? ProbeRoot(string root)
    {
        string[] candidates =
        {
            Path.Combine(root, "CXK", "abi", "cxk_abi.h"),
            Path.Combine(root, "abi", "cxk_abi.h"),
        };
        foreach (string c in candidates)
        {
            if (File.Exists(c)) return Path.GetFullPath(c);
        }
        return null;
    }
}
