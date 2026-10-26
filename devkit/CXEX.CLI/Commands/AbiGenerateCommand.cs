// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using CXEX.Lang.Abi;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Writes <c>abi/cxk_abi.h</c> (roadmap step 6a1) and the kernel's syscall
/// table, <c>kernel/cpu/systable.h</c> and <c>.c</c> (6a2), from <c>abi/kernel.xfil</c>.
/// Reads the file on disk, not the copy built into this <c>cxk</c>, so an edit
/// to the interface can be generated and compiled before the DevKit is rebuilt;
/// <c>check-abi</c> then reports the DevKit as stale until it is, because the X
/// prelude comes from the built-in copy.
/// </summary>
public class AbiGenerateCommand : Command<AbiGenerateCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "[XFIL]")]
        [Description("Path to kernel.xfil. Default: abi/kernel.xfil of the checkout, found as check-abi finds it.")]
        public string? XfilPath { get; set; }

        [CommandOption("--root")]
        [Description("Repository root the generated files are written under. Default: the directory above the .xfil's.")]
        public string? Root { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        string? xfil = settings.XfilPath is { Length: > 0 }
            ? Path.GetFullPath(settings.XfilPath)
            : CheckAbiCommand.LocateHeader() is { } h ? Path.Combine(Path.GetDirectoryName(h)!, "kernel.xfil") : null;
        if (xfil is null || !File.Exists(xfil))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] could not find kernel.xfil{(xfil is null ? "" : $" at {Markup.Escape(xfil)}")}.");
            return 1;
        }

        XfilFile file;
        try { file = XfilParser.Parse(File.ReadAllText(xfil)); }
        catch (XfilException e)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {Markup.Escape(Path.GetFileName(xfil))}, {Markup.Escape(e.Message)}");
            return 1;
        }

        string root = settings.Root is { Length: > 0 }
            ? Path.GetFullPath(settings.Root)
            : Path.GetDirectoryName(Path.GetDirectoryName(xfil)!)!;
        foreach ((string rel, string text) in XfilEmit.Outputs(file))
        {
            string outPath = Path.Combine(root, rel);
            bool same = File.Exists(outPath) && File.ReadAllText(outPath).Replace("\r\n", "\n") == text;
            if (!same)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(outPath)!);
                File.WriteAllBytes(outPath, new UTF8Encoding(false).GetBytes(text));
            }
            AnsiConsole.MarkupLine($"{(same ? "[grey]unchanged:[/]" : "[green]wrote:[/]")} {Markup.Escape(outPath)}");
        }
        AnsiConsole.MarkupLine($"[grey]{file.Syscalls.Count()} syscalls, {file.Structs.Count()} structures[/]");
        return 0;
    }
}
