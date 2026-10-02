using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Holds versions.json to the tree: every version it declares must equal the
/// version actually written wherever it is written.
///
/// This is engineering review §13 ("version and format compatibility should be
/// centralized") with teeth. Before it, the numbers had already drifted in three
/// ways nobody could see: the VSIX said 0.5.0 while the CLI said 5.0.0 and Studio
/// defaulted to 1.0.0; the DevKit refused a CXEX format_version of 2 while the
/// kernel validated it not at all; and every image was stamped abi_version 1,
/// with FLAG_REQUIRE_ABI_MATCH set, against a contract documented as v2 - and
/// cxex_check_compat, the function that would have caught it, had no caller.
///
/// The checker is deliberately DATA-DRIVEN. Each check in versions.json names a
/// file and a regex whose first capture group must equal the declared version, so
/// a new place a version is written is a new entry in that file and no change
/// here. A checker that had to be edited to learn about a new site is a checker
/// that stops covering the sites nobody remembered to add.
/// </summary>
public class CheckVersionsCommand : Command<CheckVersionsCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandOption("--root <DIR>")]
        [Description("Repository root. Default: found by walking up from the working directory.")]
        public string? Root { get; set; }

        [CommandOption("--quiet")]
        [Description("Print only failures and the one-line verdict.")]
        public bool Quiet { get; set; }
    }

    private sealed record Check(string Kind, string Name, string Declared, string File, string Pattern);

    protected override int Execute(CommandContext context, Settings s, CancellationToken ct)
    {
        string? root = s.Root is { Length: > 0 } r ? Path.GetFullPath(r) : FindRoot();
        if (root is null)
        {
            AnsiConsole.MarkupLine("[red]error:[/] could not find the repository root.");
            return 1;
        }

        string registryPath = Path.Combine(root, "versions.json");
        if (!File.Exists(registryPath))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] version registry not found: {registryPath}");
            return 1;
        }

        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(File.ReadAllText(registryPath),
                new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
        }
        catch (JsonException e)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] versions.json is not valid JSON: {e.Message}");
            return 1;
        }

        var checks = new List<Check>();
        int declared = 0;
        foreach (string kind in new[] { "components", "formats" })
        {
            if (!doc.RootElement.TryGetProperty(kind, out var group)) continue;
            foreach (var entry in group.EnumerateObject())
            {
                if (!entry.Value.TryGetProperty("version", out var v)) continue;
                // A semver is a string, a format version a bare integer. Both end
                // up compared as text, which is what the capture group gives us.
                string value = v.ValueKind == JsonValueKind.Number ? v.GetRawText() : v.GetString() ?? "";
                declared++;

                if (!entry.Value.TryGetProperty("checks", out var list)) continue;
                foreach (var c in list.EnumerateArray())
                {
                    string? file = c.TryGetProperty("file", out var f) ? f.GetString() : null;
                    string? pat = c.TryGetProperty("pattern", out var p) ? p.GetString() : null;
                    if (file is null || pat is null) continue;
                    checks.Add(new Check(kind, entry.Name, value, file, pat));
                }
            }
        }

        if (checks.Count == 0)
        {
            AnsiConsole.MarkupLine("[yellow]warning:[/] versions.json declares no checks — nothing was verified.");
            return 1;
        }

        int failed = 0;
        foreach (var c in checks)
        {
            string full = Path.Combine(root, c.File.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(full))
            {
                AnsiConsole.MarkupLine($"[red]MISSING[/] {c.Kind}/{c.Name}: no such file: {c.File}");
                failed++;
                continue;
            }

            Match m;
            try
            {
                m = Regex.Match(File.ReadAllText(full), c.Pattern);
            }
            catch (ArgumentException e)
            {
                AnsiConsole.MarkupLine($"[red]BAD PATTERN[/] {c.Kind}/{c.Name} in {c.File}: {e.Message}");
                failed++;
                continue;
            }

            // Not found is a FAILURE, never a pass. A check that silently matches
            // nothing is worse than no check: it reports green for a file that
            // may say anything at all, which is exactly how a registry rots.
            if (!m.Success || m.Groups.Count < 2)
            {
                AnsiConsole.MarkupLine($"[red]NOT FOUND[/] {c.Kind}/{c.Name}: pattern matched nothing in {c.File}");
                AnsiConsole.MarkupLine($"[grey]  pattern: {Markup.Escape(c.Pattern)}[/]");
                failed++;
                continue;
            }

            string found = m.Groups[1].Value;
            if (found != c.Declared)
            {
                AnsiConsole.MarkupLine($"[red]MISMATCH[/] {c.Kind}/{c.Name}: {c.File}");
                AnsiConsole.MarkupLine($"[grey]  registry says {c.Declared}, file says {found}[/]");
                failed++;
                continue;
            }

            if (!s.Quiet)
                AnsiConsole.MarkupLine($"[green]ok[/] {c.Kind}/{c.Name} [grey]{c.Declared}[/] — {c.File}");
        }

        AnsiConsole.MarkupLine("");
        if (failed > 0)
        {
            AnsiConsole.MarkupLine($"[red]version check FAILED:[/] {failed} of {checks.Count} site(s) disagree with versions.json.");
            AnsiConsole.MarkupLine("[grey]  Decide the version in versions.json, then make the tree match it — not the other way round.[/]");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]version check passed:[/] {declared} versions declared, {checks.Count} site(s) agree.");
        return 0;
    }

    /// <summary>Walk up looking for the registry beside the other repo markers.</summary>
    private static string? FindRoot()
    {
        var d = new DirectoryInfo(Directory.GetCurrentDirectory());
        while (d is not null)
        {
            if (File.Exists(Path.Combine(d.FullName, "versions.json")) &&
                Directory.Exists(Path.Combine(d.FullName, "abi")))
                return d.FullName;
            d = d.Parent;
        }
        return null;
    }
}
