// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Xml.Linq;
using Xunit;
using CXEX.Tests;

namespace CXEX.Tests.Unit;

/// <summary>
/// Library → front-end layering (DevKit engineering §8), asserted rather than
/// agreed.
///
/// <para>Studio referenced <c>CXEX.CLI</c> — the whole command-line front end,
/// Spectre.Console and the command registrar included — in order to launch an
/// emulator, because <c>QemuTool</c> lived in <c>CXEX.CLI/Wrappers</c>. Moving
/// the wrappers to <c>CXEX.Tools</c> (§6) is what let that reference go, which
/// is why the two review items were one piece of work.</para>
///
/// <para>A rule nothing checks is a rule that comes back with the next feature
/// that finds a useful class in the wrong assembly, so these read the project
/// files.</para>
/// </summary>
public class ToolsLayeringTests
{
    private static IEnumerable<string> ProjectReferences(string csprojPath)
    {
        var doc = XDocument.Load(csprojPath);
        return doc.Descendants("ProjectReference")
                  .Select(e => e.Attribute("Include")?.Value ?? string.Empty)
                  .Select(v => Path.GetFileNameWithoutExtension(v.Replace('\\', '/')))
                  .Where(v => v.Length > 0)
                  .ToList();
    }

    /// <summary>
    /// The file's code, with comment lines removed.
    ///
    /// <para>These rules are about what the code <i>does</i>, and a comment
    /// explaining why something is absent must not count as its presence.
    /// Both of the scans below failed on their first run for exactly that
    /// reason - on <c>ToolProcess</c>'s own doc comment, which says that the
    /// runner no longer writes to <c>AnsiConsole</c> and that <c>BochsTool</c>
    /// is gone.</para>
    ///
    /// <para>Line-based, so it handles <c>//</c> and the usual block-comment
    /// layout but not a <c>/* ... */</c> opened mid-line. That is enough here
    /// and the limit is worth stating: a trailing comment on a line of real
    /// code is still scanned, which errs towards reporting.</para>
    /// </summary>
    private static string CodeOnly(string path)
    {
        var sb = new System.Text.StringBuilder();
        bool inBlock = false;

        foreach (string raw in File.ReadLines(path))
        {
            string line = raw.TrimStart();

            if (inBlock)
            {
                if (line.Contains("*/")) inBlock = false;
                continue;
            }
            if (line.StartsWith("/*")) { if (!line.Contains("*/")) inBlock = true; continue; }
            if (line.StartsWith("//") || line.StartsWith("*")) continue;

            sb.AppendLine(raw);
        }

        return sb.ToString();
    }

    private static string? Csproj(string project)
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) return null;
        string path = Path.Combine(repo, "devkit", project, $"{project}.csproj");
        return File.Exists(path) ? path : null;
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Studio_does_not_reference_the_cli()
    {
        string? studio = Csproj("CXEX.Studio");
        if (studio is null) { Assert.True(true, "no checkout: skipped"); return; }

        var refs = ProjectReferences(studio).ToList();

        // The test is only meaningful if it parsed something.
        Assert.NotEmpty(refs);
        Assert.Contains("CXEX.Tools", refs);

        Assert.DoesNotContain("CXEX.CLI", refs);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_tools_library_depends_on_no_other_cxex_project()
    {
        string? tools = Csproj("CXEX.Tools");
        if (tools is null) { Assert.True(true, "no checkout: skipped"); return; }

        // CXEX.Tools runs external processes and nothing else. Keeping it at
        // the bottom of the graph is what lets both front ends use it; a
        // reference to Core or FileType here would be the first step back to
        // the tangle that put process launching inside the CLI.
        Assert.Empty(ProjectReferences(tools));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_tools_library_does_not_write_to_a_console()
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) { Assert.True(true, "no checkout: skipped"); return; }

        string dir = Path.Combine(repo, "devkit", "CXEX.Tools");
        if (!Directory.Exists(dir)) { Assert.True(true, "not found: skipped"); return; }

        var offenders = new List<string>();
        foreach (string file in Directory.GetFiles(dir, "*.cs", SearchOption.AllDirectories))
        {
            string text = CodeOnly(file);
            if (text.Contains("Spectre.Console") ||
                text.Contains("Console.WriteLine") ||
                text.Contains("AnsiConsole"))
                offenders.Add(Path.GetFileName(file));
        }

        // Output leaves through the ToolRunOptions callbacks. A library that
        // writes to a console is a front end wearing a library's name, and it
        // is also what made this code un-shareable in the first place.
        Assert.Empty(offenders);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Bochs_support_is_gone_from_the_devkit()
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) { Assert.True(true, "no checkout: skipped"); return; }

        string devkit = Path.Combine(repo, "devkit");
        if (!Directory.Exists(devkit)) { Assert.True(true, "not found: skipped"); return; }

        // Removed 2026-10-09: QEMU is the only emulator CXOS targets, and a
        // second emulator path nothing exercised was a maintenance claim the
        // project was not making good on. `cxk run -e bochs` still answers,
        // naming it as removed rather than as an unknown emulator, so this
        // allows the word in that one message and in explanatory comments -
        // what it forbids is a BochsTool or a BochsConfig coming back.
        var offenders = new List<string>();
        foreach (string file in Directory.GetFiles(devkit, "*.cs", SearchOption.AllDirectories))
        {
            if (file.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}") ||
                file.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"))
                continue;
            if (Path.GetFileName(file) == "ToolsLayeringTests.cs") continue;

            string text = CodeOnly(file);
            if (text.Contains("BochsTool") || text.Contains("BochsConfig"))
                offenders.Add(Path.GetFileName(file));
        }

        Assert.Empty(offenders);
    }
}
