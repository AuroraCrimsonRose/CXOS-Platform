// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The x86-64 back end (roadmap step 6b3), run: <c>cxk compile --target
/// x86_64</c> lowers to the IR and writes System V AMD64 assembly, linked
/// here into a 64-bit Linux process. The programs <see cref="LangRunTests"/>
/// runs on x86-32 must return 0 on x86-64 too; X and C must call each other
/// both ways, under clang's System V as well as through the corpus x86-32
/// shares; and <c>std/buf</c> must work against its refusing allocator.
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class X64RunTests
{
    [Fact]
    public void Programs_that_must_run_all_return_zero_on_x86_64()
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        /* and tests/x64/run: what only x86-64 Linux runs, its syscalls */
        var progs = Sorted(Path.Combine(tests, "lang", "run")).Concat(Sorted(Path.Combine(tests, "ir", "run")))
            .Concat(Sorted(Path.Combine(tests, "x64", "run"))).ToList();
        Assert.True(progs.Count >= 17, $"only {progs.Count} programs found");

        var failures = new List<string>();
        foreach (string src in progs)
        {
            string name = Path.GetFileName(src);
            string baseName = Path.Combine(work.Path, Path.GetFileNameWithoutExtension(src));

            var r = Compile(cxk, repo, src, baseName + ".elf", "--emit-asm");
            if (!File.Exists(baseName + ".s")) { failures.Add($"{name}: did not compile\n{r.All}"); continue; }

            HostBuild.Link64(baseName + ".s", baseName + ".bin", work.Path);
            var x = HostBuild.Run(baseName + ".bin", Array.Empty<string>(), work.Path, 120_000);
            if (x.ExitCode != 0) failures.Add($"{name}: check {x.ExitCode} failed");
        }

        Assert.True(failures.Count == 0,
            $"{failures.Count} of {progs.Count} programs failed on x86-64:\n  " + string.Join("\n  ", failures));
    }

    [Theory]
    [InlineData("interop")]     // the corpus x86-32 runs too
    [InlineData("interop64")]   // System V: stack arguments, i128, aggregates in memory
    public void C_and_X_interoperate_on_x86_64(string corpus)
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        string xSrc = Path.Combine(tests, "lang", corpus, "x.xfxn");
        string cSrc = Path.Combine(tests, "lang", corpus, "c.c");
        Assert.SkipWhen(!File.Exists(xSrc) || !File.Exists(cSrc), $"the {corpus} corpus is not in this checkout.");

        string xo = Path.Combine(work.Path, "x.o");
        var r = Compile(cxk, repo, xSrc, xo, "--object");
        Assert.True(File.Exists(xo), $"X did not compile:\n{r.All}");

        string stub = Path.Combine(work.Path, "stub64.s");
        File.WriteAllText(stub, HostBuild.PlainStub64);
        string exe = Path.Combine(work.Path, corpus + ".bin");

        var link = HostBuild.Run(HostBuild.HostCc, new[]
        {
            "-m64", "-O2", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-stack-protector",
            "-nostdlib", "-static", "-o", exe, cSrc, xo, stub,
        }, work.Path);
        Assert.True(link.ExitCode == 0, $"linking C with X failed:\n{link.All}");

        var x = HostBuild.Run(exe, Array.Empty<string>(), work.Path, 120_000);
        Assert.True(x.ExitCode == 0, $"{corpus} (C <-> X) on x86-64: check {x.ExitCode} failed");
    }

    [Fact]
    public void Std_buf_works_on_x86_64()
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        string? stdDir = Path.Combine(repo, "os", "std");
        if (!File.Exists(Path.Combine(stdDir, "buf.xfxn"))) stdDir = null;
        Assert.SkipWhen(stdDir is null, "no CXOS checkout with os/std/buf.xfxn (set CXK_ROOT).");

        string sd = Path.Combine(work.Path, "std");
        Directory.CreateDirectory(sd);
        foreach (string f in new[] { "buf.xfxn", "mem.xfxn" })
            File.Copy(Path.Combine(stdDir!, f), Path.Combine(sd, f), overwrite: true);
        foreach (string f in new[] { "heap.xfxn", "buf_test.xfxn" })
        {
            string from = Path.Combine(tests, "lang", "std", f);
            Assert.SkipWhen(!File.Exists(from), $"the std corpus is not in this checkout ({f}).");
            File.Copy(from, Path.Combine(sd, f), overwrite: true);
        }

        string baseName = Path.Combine(sd, "buf_test");
        var r = Compile(cxk, repo, baseName + ".xfxn", baseName + ".elf", "--emit-asm");
        Assert.True(File.Exists(baseName + ".s"), $"std/buf did not compile:\n{r.All}");

        HostBuild.Link64(baseName + ".s", baseName + ".bin", work.Path);
        var x = HostBuild.Run(baseName + ".bin", Array.Empty<string>(), work.Path, 120_000);
        Assert.True(x.ExitCode == 0, $"std/buf.xfxn on x86-64: check {x.ExitCode} failed");
    }

    private static (string Repo, string Cxk, HostBuild.Workspace Work, string Tests) Setup()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();
        return (repo, cxk, new HostBuild.Workspace("x64"), Path.Combine(repo, "devkit", "tests"));
    }

    private static ProcResult Compile(string cxk, string repo, string src, string outPath, params string[] extra) =>
        HostBuild.Run(cxk, new[] { "compile", src, outPath, "--no-prelude", "--target", "x86_64" }.Concat(extra), repo);

    private static List<string> Sorted(string dir) =>
        Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, "*.xfxn").OrderBy(p => p, StringComparer.Ordinal).ToList()
            : new List<string>();
}
