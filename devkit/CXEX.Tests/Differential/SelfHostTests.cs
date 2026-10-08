// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The X compiler compiles itself, twice, and the result must be a fixed
/// point. Port of <c>devkit/tests/xc/selfhost.py</c> (HARDENING_PLAN D1).
///
/// <para>Three stages:</para>
/// <list type="number">
///   <item>the C# compiler builds <c>xc</c> → <c>xc1</c></item>
///   <item><c>xc1</c> builds <c>xc</c> → <c>xc2</c></item>
///   <item><c>xc2</c> builds <c>xc</c> → stage 3 assembly</item>
/// </list>
///
/// <para>Stage 2 must equal stage 1, which says the X compiler agrees with the
/// oracle on its own source; stage 3 must equal stage 2, which says the
/// compiler is a fixed point under itself. Both matter: the first catches the
/// two compilers drifting apart, the second catches a compiler that miscompiles
/// itself into something that still runs.</para>
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class SelfHostTests
{
    [Fact]
    public void Xc_compiles_itself_to_a_fixed_point()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();

        string? osDir = HostBuild.FindOsDir(repo, "xc");
        Assert.SkipWhen(osDir is null,
            "no CXOS checkout with os/xc/xc.xfxn was found. Set CXK_ROOT to the repository root.");

        using var work = new HostBuild.Workspace("selfhost");
        string src = Path.Combine(osDir!, "xc", "xc.xfxn");
        string[] inc =
        {
            "-I", Path.Combine(osDir!, "xc", "host"),
            "-I", Path.Combine(osDir!, "xc"),
            "-I", Path.Combine(osDir!, "std"),
        };

        // ---- stage 1: the C# compiler builds xc ----
        string s1 = Path.Combine(work.Path, "xc1.s");
        var r = HostBuild.Run(cxk,
            new[] { "compile", src, Path.Combine(work.Path, "xc1.elf"), "--no-prelude", "--emit-asm" }.Concat(inc),
            repo);
        Assert.True(File.Exists(s1), $"stage 1 did not compile:\n{r.All}");
        string a1 = File.ReadAllText(s1);
        HostBuild.Link(s1, Path.Combine(work.Path, "xc1"), HostBuild.ArgvStub, work.Path);

        // ---- stage 2: xc1 builds xc ----
        string s2 = Path.Combine(work.Path, "xc2.s");
        RunXc(Path.Combine(work.Path, "xc1"), inc, s2, src, work.Path);
        string a2 = File.ReadAllText(s2);
        HostBuild.Link(s2, Path.Combine(work.Path, "xc2"), HostBuild.ArgvStub, work.Path);

        // ---- stage 3: xc2 builds xc ----
        string s3 = Path.Combine(work.Path, "xc3.s");
        RunXc(Path.Combine(work.Path, "xc2"), inc, s3, src, work.Path);
        string a3 = File.ReadAllText(s3);

        // All three are the compilers' own output: Link() renames the entry
        // point into a sidecar and leaves these files alone, so this compares
        // what was actually emitted.
        Console.WriteLine($"stage 1 (C#): {a1.Split('\n').Length} lines");
        Assert.True(a2 == a1, Describe("stage 2 (xc by C#) differs from stage 1 (C#)", a1, a2));
        Assert.True(a3 == a2, Describe("stage 3 (xc by xc) differs from stage 2", a2, a3));

        // ---- and what the self-compiled compiler builds must actually work ----
        string runStub = Path.Combine(work.Path, "stub.s");
        File.WriteAllText(runStub, HostBuild.PlainStub);

        var progs = Directory
            .EnumerateFiles(Path.Combine(repo, "devkit", "tests", "lang", "run"), "*.xfxn")
            .OrderBy(p => p, StringComparer.Ordinal)
            .ToList();
        Assert.SkipWhen(progs.Count == 0, "no programs in devkit/tests/lang/run to run.");

        var failures = new List<string>();
        foreach (string p in progs)
        {
            string name = Path.GetFileNameWithoutExtension(p);
            string baseName = Path.Combine(work.Path, name);

            var c = HostBuild.Run(Path.Combine(work.Path, "xc2"),
                                  new[] { "--no-prelude", p, "-o", baseName + ".s" }, work.Path);
            if (c.ExitCode != 0 || !File.Exists(baseName + ".s"))
            {
                failures.Add($"{name}: xc2 did not compile it\n{c.All}");
                continue;
            }

            HostBuild.Link(baseName + ".s", baseName + ".bin", HostBuild.PlainStub, work.Path);
            var x = HostBuild.Run(baseName + ".bin", Array.Empty<string>(), work.Path, 120_000);
            if (x.ExitCode != 0) failures.Add($"{name}: exit {x.ExitCode}");
        }

        Assert.True(failures.Count == 0,
            $"{failures.Count} of {progs.Count} programs built by xc2 failed:\n  " +
            string.Join("\n  ", failures));

        Console.WriteLine($"tests/lang/run compiled by xc2: {progs.Count} of {progs.Count} run correctly");
    }

    private static void RunXc(string compiler, string[] inc, string outPath, string src, string workDir)
    {
        var r = HostBuild.Run(compiler,
            new[] { "--no-prelude" }.Concat(inc).Concat(new[] { src, "-o", outPath }), workDir);
        Assert.True(r.ExitCode == 0, $"{Path.GetFileName(compiler)} failed:\n{r.All}");
        Assert.True(File.Exists(outPath), $"{Path.GetFileName(compiler)} produced no output at {outPath}");
    }

    private static string Describe(string what, string expected, string actual)
    {
        var e = expected.Split('\n');
        var a = actual.Split('\n');
        int i = 0;
        while (i < e.Length && i < a.Length && e[i] == a[i]) i++;
        return $"{what}, first at line {i}:\n" +
               $"  expected: {(i < e.Length ? e[i] : "<end>")}\n" +
               $"  actual:   {(i < a.Length ? a[i] : "<end>")}";
    }
}
