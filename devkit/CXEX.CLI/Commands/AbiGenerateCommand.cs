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
/// Writes <c>abi/cxk_abi.h</c> from <c>abi/kernel.xfil</c> (roadmap step 6a1).
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

        [CommandOption("-o|--out")]
        [Description("Where to write the header. Default: cxk_abi.h beside the .xfil.")]
        public string? Out { get; set; }
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

        string outPath = settings.Out is { Length: > 0 }
            ? Path.GetFullPath(settings.Out)
            : Path.Combine(Path.GetDirectoryName(xfil)!, "cxk_abi.h");
        string header = XfilEmit.CHeader(file);
        bool same = File.Exists(outPath) && File.ReadAllText(outPath).Replace("\r\n", "\n") == header;
        if (!same) File.WriteAllBytes(outPath, new UTF8Encoding(false).GetBytes(header));

        AnsiConsole.MarkupLine(
            $"{(same ? "[grey]unchanged:[/]" : "[green]wrote:[/]")} {Markup.Escape(outPath)} " +
            $"[grey]({file.Syscalls.Count()} syscalls, {file.Structs.Count()} structures)[/]");
        return 0;
    }
}
