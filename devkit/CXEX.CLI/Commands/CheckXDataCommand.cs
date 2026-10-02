// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading;
using CXEX.Lang.Data;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Pre-flight: check X Data documents - service descriptors today - before they go on a disk.
///
/// <para>Without this, a typo in a <c>.xosv</c> is found at boot, by the supervisor, as one line
/// in a log. With it, the build stops and names the file, line and column. The reader is
/// <see cref="XData"/>, a port of the X reader the supervisor uses, so a document this accepts
/// the supervisor accepts too: same rules, same error codes, same offsets.</para>
///
/// <para><c>--keys</c> additionally refuses top-level keys outside a known set, which is how a
/// misspelt <c>grnats</c> becomes a build error instead of a silently ignored line.</para>
///
/// <para>Exit code 0 means every file passed; 1 means at least one did not.</para>
/// </summary>
public class CheckXDataCommand : Command<CheckXDataCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<FILES>")]
        [Description("X Data documents to check.")]
        public string[] Files { get; set; } = Array.Empty<string>();

        [CommandOption("--keys <KEYS>")]
        [Description("Comma-separated list of the top-level keys allowed; any other key is an error.")]
        public string? Keys { get; set; }

        [CommandOption("--porcelain")]
        [Description("Machine output, one line per file: <file>\\t<code>\\t<offset>. Code 0 is valid; " +
                     "negative codes are the XD_E_* numbers shared with the X reader.")]
        public bool Porcelain { get; set; }

        [CommandOption("--quiet")]
        [Description("Only print failures.")]
        public bool Quiet { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        HashSet<string>? allowed = null;
        if (settings.Keys is { Length: > 0 })
        {
            allowed = settings.Keys
                .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .ToHashSet(StringComparer.Ordinal);
        }

        int failed = 0;
        foreach (string file in settings.Files)
        {
            if (!File.Exists(file))
            {
                failed++;
                if (settings.Porcelain) Console.WriteLine($"{file}\tmissing\t0");
                else Console.WriteLine($"{file}: error: not found");
                continue;
            }

            var doc = new XData(File.ReadAllBytes(file));
            XDataError code = doc.Check();

            if (settings.Porcelain)
            {
                // Syntax only: the differential test compares exactly this against the X reader,
                // so nothing layered on top may change it.
                Console.WriteLine($"{file}\t{(int)code}\t{(code == XDataError.Ok ? 0 : doc.ErrorAt)}");
                if (code != XDataError.Ok) failed++;
                continue;
            }

            if (code != XDataError.Ok)
            {
                failed++;
                Report(file, doc, doc.ErrorAt, XData.Describe(code));
                continue;
            }

            bool bad = false;
            if (allowed is not null)
            {
                foreach (var (key, keyAt, _) in doc.Entries(doc.Root))
                {
                    if (allowed.Contains(key)) continue;
                    Report(file, doc, keyAt,
                        $"unknown key '{key}' (known: {string.Join(", ", allowed.OrderBy(k => k, StringComparer.Ordinal))})");
                    bad = true;
                }
            }

            if (bad) failed++;
            else if (!settings.Quiet) AnsiConsole.MarkupLine($"[green]ok:[/] {Markup.Escape(file)}");
        }

        if (settings.Porcelain) return failed > 0 ? 1 : 0;

        if (failed > 0)
        {
            AnsiConsole.MarkupLine($"[red]X Data check failed:[/] {failed} of {settings.Files.Length} file(s).");
            return 1;
        }
        if (!settings.Quiet)
            AnsiConsole.MarkupLine($"[green]X Data check passed:[/] {settings.Files.Length} file(s).");
        return 0;
    }

    // file:line:col - the shape editors and CI logs already know how to jump to. Written
    // plainly rather than through Spectre, which wraps at the console width and would split
    // the line a log parser is looking for.
    private static void Report(string file, XData doc, int at, string message)
    {
        var (line, col) = doc.Position(at);
        Console.WriteLine($"{file}:{line}:{col}: error: {message}");
    }
}
