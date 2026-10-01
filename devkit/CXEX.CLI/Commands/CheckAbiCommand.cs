using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using CXEX.Lang.Abi;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Pre-flight: confirm the X ABI prelude still matches the kernel's ABI header.
///
/// <para>The prelude (<c>CXEX.Lang/Abi/AbiPrelude.cs</c>) is a hand-maintained copy of
/// <c>abi/cxk_abi.h</c> that claims in its own banner to be generated. Nothing generates
/// it and the two live in different repositories, so it drifts silently - and when it drifts,
/// the toolchain cannot compile the userland. That has already happened: the kernel gained
/// <c>SYS_MOUSE_READ</c> and <c>struct mouse_state</c>, the prelude did not, and
/// <c>gui.xfxn</c> used both.</para>
///
/// <para>Run this before building CXK's userland. Exit code 0 means the mirrored surface
/// agrees; 1 means it does not. The comparison itself lives in
/// <see cref="CXEX.Lang.Abi.AbiSync"/> so Studio and a future test project can share it.</para>
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
                "[grey]  Fix AbiPrelude.Generate() in CXEX.Lang/Abi/AbiPrelude.cs, then re-run.[/]");
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
                : "[green]ABI check passed:[/] the prelude matches cxk_abi.h.");
        return 0;
    }

    /// <summary>
    /// Finds cxk_abi.h without being told. In order: <c>CXK_ROOT</c>, then a sibling CXK
    /// checkout walking up from the working directory. The header sits at
    /// <c>&lt;repo&gt;/abi/cxk_abi.h</c> - the repository root contains a nested CXK
    /// directory - so both layouts are tried at each level.
    /// </summary>
    private static string? LocateHeader()
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
