// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// os/std/bidi.xfxn, the UAX #9 implementation the toolkit draws text with,
/// against Unicode's own conformance files (vendor/unicode): every case of
/// BidiTest.txt, in each paragraph direction it names, and of
/// BidiCharacterTest.txt, levels and visual order (system/CX_TOOLKIT.md §10).
/// os/xc/bidicheck.xfxn runs them; it is built for the host as the
/// compiler's dump programs are, so it needs an ELF host like they do.
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class BidiConformanceTests
{
    [Fact]
    public void Every_case_of_Unicodes_bidi_conformance_files_passes()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();

        string? osDir = HostBuild.FindOsDir(repo, "bidicheck");
        Assert.SkipWhen(osDir is null, "no CXOS checkout with os/xc/bidicheck.xfxn was found. Set CXK_ROOT to the repository root.");

        using var work = new HostBuild.Workspace("bidicheck");
        string exe = HostBuild.BuildDumpProgram(cxk, repo, osDir!, "bidicheck", work.Path);
        string ucd = Path.Combine(repo, "vendor", "unicode");
        var r = HostBuild.Run(exe, new[]
        {
            Path.Combine(ucd, "BidiTest.txt"),
            Path.Combine(ucd, "BidiCharacterTest.txt"),
        }, repo);

        // bidicheck prints "<file>: <n> cases, <f> failed" per file, then the first failures
        Assert.True(r.ExitCode == 0, $"bidicheck exited {r.ExitCode}:\n{r.All}");
        Assert.Contains("BidiTest.txt: ", r.StdOut);
        Assert.Contains("BidiCharacterTest.txt: ", r.StdOut);
        Assert.Contains(" cases, 0 failed", r.StdOut);
        // and the counts are Unicode 18.0.0's files whole (BidiTest.txt's cases
        // counted once a direction), so a misread file cannot pass by being short
        Assert.Contains("BidiTest.txt: 770241 cases, 0 failed", r.StdOut);
        Assert.Contains("BidiCharacterTest.txt: 91707 cases, 0 failed", r.StdOut);
    }
}
