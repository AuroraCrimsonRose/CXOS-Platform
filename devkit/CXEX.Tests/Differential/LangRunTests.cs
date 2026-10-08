// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The X language itself: programs that must run and return 0, programs the
/// compiler must refuse with a specific message, C↔X interop, and
/// <c>std/buf</c> against an allocator that needs no kernel. Port of
/// <c>devkit/tests/lang/run.py</c> (HARDENING_PLAN D1).
///
/// <para><c>tests/lang/refuse</c> is the valuable half: every program in it
/// once compiled and produced a wrong answer, so each is pinned to the
/// diagnostic that must appear. A refusal is not enough on its own - refusing
/// for the wrong reason would pass a looser test.</para>
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class LangRunTests
{
    /// <summary>
    /// What each refusal must say. Checked by substring, so wording can grow
    /// around these without breaking, but the diagnosis cannot change silently.
    /// </summary>
    private static readonly Dictionary<string, string[]> Expected = new()
    {
        ["narrow.xfxn"] = new[] { "narrowing needs a cast: `as u32`" },
        ["shift_width.xfxn"] = new[] { "shift of 100 is outside 0..31", "shift of 40 is outside 0..31" },
        ["suffix_range.xfxn"] = new[] { "256 does not fit in u8", "128 does not fit in i8", "129 does not fit in i8" },
        ["array_param.xfxn"] = new[] { "is an array; pass a pointer" },
        ["char_literal.xfxn"] = new[] { "a character literal is one ASCII character or escape" },
        ["compound_call.xfxn"] = new[] { "contains a call, which would run twice" },
        ["initializers.xfxn"] = new[]
        {
            "expected a constant expression", "field 'y' of 'pt' is not given", "'pt' has no field 'z'",
            "field 'x' given twice", "[3]u32 needs 3 item(s), given 2",
            "an array literal needs a declared array type",
        },
        ["enums_switch.xfxn"] = new[]
        {
            "'tiny.b' = 256 does not fit in u8", "'dup.y' has the same value as 'x'",
            "cannot initialize 'c' of type color from i32", "cannot initialize 'n' of type u32 from color",
            "switch does not handle color.blue", "'color.red' appears in two cases",
            "'node.num' carries fields", "'node.eof' carries no fields; write it without braces",
            "'node.eof' carries no fields to bind", "a sum type's fields are reached through a switch",
            "pt cannot be compared with ==", "this value appears in two cases",
        },
        ["const_cycle.xfxn"] = new[] { "is defined in terms of itself" },
        ["sum_cycle.xfxn"] = new[] { "'tree' contains 'tree' by value inside itself, so it has no size" },
        ["too_large.xfxn"] = new[]
        {
            "'loop' contains itself by value, so it has no size", "'b' contains itself by value",
            "huge is larger than 1 GB", "[2147483647]u32 is larger than 1 GB",
            "[1073741825]u8 is larger than 1 GB",
        },
        ["conversions.xfxn"] = new[]
        {
            "u32 to u8 can change the value", "300 does not fit in u8",
            "u32 to i32 can change the value", "i8 to u32 can change the value",
            "compilation failed (4 error(s))",
        },
    };

    [Fact]
    public void Programs_that_must_run_all_return_zero()
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        var progs = Sorted(Path.Combine(tests, "lang", "run"));
        Assert.SkipWhen(progs.Count == 0, "no programs in devkit/tests/lang/run.");

        var failures = new List<string>();
        foreach (string src in progs)
        {
            string name = Path.GetFileName(src);
            string baseName = Path.Combine(work.Path, Path.GetFileNameWithoutExtension(src));

            var r = Compile(cxk, repo, src, baseName + ".elf", "--emit-asm");
            if (!File.Exists(baseName + ".s")) { failures.Add($"{name}: did not compile\n{r.All}"); continue; }

            HostBuild.Link(baseName + ".s", baseName + ".bin", HostBuild.PlainStub, work.Path);
            var x = HostBuild.Run(baseName + ".bin", Array.Empty<string>(), work.Path, 120_000);
            if (x.ExitCode != 0) failures.Add($"{name}: check {x.ExitCode} failed");
        }

        Assert.True(failures.Count == 0,
            $"{failures.Count} of {progs.Count} programs failed:\n  " + string.Join("\n  ", failures));
    }

    [Fact]
    public void Programs_that_must_be_refused_are_refused_for_the_right_reason()
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        var progs = Sorted(Path.Combine(tests, "lang", "refuse"));
        Assert.SkipWhen(progs.Count == 0, "no programs in devkit/tests/lang/refuse.");

        var failures = new List<string>();
        foreach (string src in progs)
        {
            string name = Path.GetFileName(src);
            var r = Compile(cxk, repo, src, Path.Combine(work.Path, Path.GetFileNameWithoutExtension(src) + ".elf"));
            string output = r.All;

            if (!output.Contains("compilation failed", StringComparison.Ordinal))
            {
                failures.Add($"refuse/{name}: compiled, and must not have");
                continue;
            }

            var missing = Expected.TryGetValue(name, out var wanted)
                ? wanted.Where(m => !output.Contains(m, StringComparison.Ordinal)).ToList()
                : new List<string>();

            if (missing.Count > 0)
                failures.Add($"refuse/{name}: refused, but without: {string.Join(" | ", missing)}");
        }

        Assert.True(failures.Count == 0,
            $"{failures.Count} of {progs.Count} refusals wrong:\n  " + string.Join("\n  ", failures));
    }

    [Fact]
    public void C_and_X_interoperate()
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        string xSrc = Path.Combine(tests, "lang", "interop", "x.xfxn");
        string cSrc = Path.Combine(tests, "lang", "interop", "c.c");
        Assert.SkipWhen(!File.Exists(xSrc) || !File.Exists(cSrc), "the interop corpus is not in this checkout.");

        string xo = Path.Combine(work.Path, "x.o");
        var r = Compile(cxk, repo, xSrc, xo, "--object");
        Assert.True(File.Exists(xo), $"X did not compile:\n{r.All}");

        string stub = Path.Combine(work.Path, "stub.s");
        File.WriteAllText(stub, HostBuild.PlainStub);
        string exe = Path.Combine(work.Path, "interop.bin");

        var link = HostBuild.Run(HostBuild.HostCc, new[]
        {
            "-m32", "-O2", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-stack-protector",
            "-nostdlib", "-static", "-o", exe, cSrc, xo, stub,
        }, work.Path);
        Assert.True(link.ExitCode == 0, $"linking C with X failed:\n{link.All}");

        var x = HostBuild.Run(exe, Array.Empty<string>(), work.Path, 120_000);
        Assert.True(x.ExitCode == 0, $"interop (C <-> X): check {x.ExitCode} failed");
    }

    [Fact]
    public void Std_buf_works_against_an_allocator_that_can_refuse()
    {
        var (repo, cxk, work, tests) = Setup();
        using var _ = work;

        string? stdDir = new[] { Path.Combine(repo, "os", "std"), Path.Combine(repo, "CXK", "os", "std") }
            .FirstOrDefault(d => File.Exists(Path.Combine(d, "buf.xfxn")));
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

        HostBuild.Link(baseName + ".s", baseName + ".bin", HostBuild.PlainStub, work.Path);
        var x = HostBuild.Run(baseName + ".bin", Array.Empty<string>(), work.Path, 120_000);
        Assert.True(x.ExitCode == 0, $"std/buf.xfxn: check {x.ExitCode} failed");
    }

    private static (string Repo, string Cxk, HostBuild.Workspace Work, string Tests) Setup()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();
        return (repo, cxk, new HostBuild.Workspace("xlang"), Path.Combine(repo, "devkit", "tests"));
    }

    private static ProcResult Compile(string cxk, string repo, string src, string outPath, params string[] extra) =>
        HostBuild.Run(cxk, new[] { "compile", src, outPath, "--no-prelude" }.Concat(extra), repo);

    private static List<string> Sorted(string dir) =>
        Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, "*.xfxn").OrderBy(p => p, StringComparer.Ordinal).ToList()
            : new List<string>();
}
