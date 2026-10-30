// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The X language itself: programs the compiler must refuse with a specific
/// message. Port of <c>devkit/tests/lang/run.py</c> (HARDENING_PLAN D1). The
/// programs that must run, C↔X interop and <c>std/buf</c> ran here on x86-32
/// until roadmap step 6d3c; <see cref="X64RunTests"/> runs the same corpora on
/// x86-64.
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
        ["usize_mix.xfxn"] = new[]
        {
            "usize to u32 needs a cast, even where the widths agree: `as u32`",
            "u32 to usize needs a cast, even where the widths agree: `as usize`",
            "usize to isize needs a cast", "u8 to usize needs a cast", "u64 to usize needs a cast",
            "cannot assign u32 to usize; u32 to usize needs a cast",
            "mixing usize and u32 needs a cast, even where the widths agree: `as usize` or `as u32`",
            "18446744073709551616 does not fit in usize",
            "compilation failed (11 error(s))",
        },
        ["atomics.xfxn"] = new[]
        {
            "'__atomic_add' needs a pointer to a 4- or 8-byte integer, not *u8",
            "'__atomic_load' needs a pointer to a 4- or 8-byte integer, not u32",
            "arg 2 to '__atomic_store': u64 not assignable to u32",
            "'__atomic_cas' expects 3 args, got 2", "'__atomic_load' expects 1 args, got 2",
            "cannot initialize 'x' of type u32 from void",
            "compilation failed (6 error(s))",
        },
        ["tls.xfxn"] = new[]
        {
            "'__tls_base' expects 0 args, got 1",
            "cannot initialize 'b' of type u32 from usize; usize to u32 needs a cast",
            "cannot initialize 'c' of type u64 from usize; usize to u64 needs a cast",
            "cannot initialize 'p' of type *u8 from usize",
            "compilation failed (4 error(s))",
        },
    };

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
