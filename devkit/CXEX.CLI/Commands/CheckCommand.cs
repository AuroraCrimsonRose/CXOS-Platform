// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;
using System.ComponentModel;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Pre-flight: confirm every source file the CMakeLists references actually
/// exists. Replaces check_sources.py.
///
/// The CMakeLists lives at &lt;root&gt;/tools/cmake/CMakeLists.txt and refers to
/// sources as ${SRC_DIR}/... or ${CXK_ROOT}/..., where both resolve to the repo
/// root (CMakeLists computes CXK_ROOT as its own dir + /../..). So source paths
/// must be resolved against the ROOT, not against the CMakeLists' own folder --
/// resolving against the folder was the old bug that reported every file missing
/// once the CMakeLists moved into tools/cmake.
/// </summary>
public class CheckCommand : Command<CheckCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<CMAKE_FILE>")]
        [Description("Path to CMakeLists.txt")]
        public string CmakePath { get; set; } = string.Empty;

        [CommandOption("--root")]
        [Description("Repo root that ${SRC_DIR}/${CXK_ROOT} resolve to. Default: auto-detected.")]
        public string? Root { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.CmakePath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{settings.CmakePath}' not found.");
            return 1;
        }

        string cmakeFull = Path.GetFullPath(settings.CmakePath);
        string cmakeDir = Path.GetDirectoryName(cmakeFull) ?? string.Empty;

        // Resolve the repo root the same way the CMakeLists does.
        string root = ResolveRoot(settings.Root, cmakeDir);

        AnsiConsole.MarkupLine($"[grey]root:[/] {root}");

        string cmakeText = File.ReadAllText(settings.CmakePath);

        // Match ${SRC_DIR}/... and ${CXK_ROOT}/... source references.
        // The (?![A-Za-z]) after the extension stops ".cmake" from matching as ".c".
        var regex = new Regex(@"\$\{(?:SRC_DIR|CXK_ROOT)\}/([^\s\)""]+\.(?:c|asm|nasm|S))(?![A-Za-z])");
        var matches = regex.Matches(cmakeText);

        int missing = 0, checkedCount = 0;
        foreach (Match match in matches)
        {
            string rel = match.Groups[1].Value;
            string full = Path.Combine(root, rel.Replace('/', Path.DirectorySeparatorChar));
            checkedCount++;
            if (!File.Exists(full))
            {
                AnsiConsole.MarkupLine($"[red]MISSING:[/] {rel}");
                missing++;
            }
        }

        if (checkedCount == 0)
        {
            AnsiConsole.MarkupLine("[yellow]warning:[/] no ${SRC_DIR}/${CXK_ROOT} source references found — nothing checked.");
            return 0;
        }

        if (missing > 0)
        {
            AnsiConsole.MarkupLine($"[red]Pre-flight failed:[/] {missing} of {checkedCount} referenced sources missing.");
            AnsiConsole.MarkupLine($"[grey]  (paths resolved against {root} — pass --root if that's wrong)[/]");
            return 1;
        }

        AnsiConsole.MarkupLine($"[green]Pre-flight passed:[/] all {checkedCount} referenced sources present.");
        return 0;
    }

    /// <summary>
    /// Repo root for ${SRC_DIR}/${CXK_ROOT}. Explicit --root wins. Otherwise:
    /// if the CMakeLists is at &lt;root&gt;/tools/cmake, root is two levels up;
    /// if it's at the repo root (legacy layout), root is its own directory.
    /// Detection: a real root contains a 'kernel' directory.
    /// </summary>
    private static string ResolveRoot(string? explicitRoot, string cmakeDir)
    {
        if (!string.IsNullOrEmpty(explicitRoot))
            return Path.GetFullPath(explicitRoot);

        // Candidate 1: two levels up (tools/cmake/CMakeLists.txt -> root)
        string twoUp = Path.GetFullPath(Path.Combine(cmakeDir, "..", ".."));
        if (Directory.Exists(Path.Combine(twoUp, "kernel")))
            return twoUp;

        // Candidate 2: the CMakeLists' own directory (legacy root layout)
        if (Directory.Exists(Path.Combine(cmakeDir, "kernel")))
            return cmakeDir;

        // Fallback: two-up is the intended new layout even if we can't confirm.
        return twoUp;
    }
}