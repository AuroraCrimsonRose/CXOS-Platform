// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.RegularExpressions;
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The toolchain pins in versions.json (security review §13, engineering §9).
///
/// <para>A floor is only as good as the parse in front of it: a pattern that
/// stops matching a tool's banner turns "too old" into "could not tell", so
/// these hold each pattern to a real banner. The rest tie the places that
/// restate a pin - the CI image, CMake's own minimum - to the one entry, so
/// raising a floor in one place and not the other fails here, not in a build
/// on somebody else's machine.</para>
/// </summary>
public class ToolchainPinTests
{
    private static IReadOnlyList<ToolPin> Pins() =>
        ToolchainPolicy.Load(Path.Combine(Requires.Repo(), "versions.json"));

    private static ToolPin Pin(string name) => Assert.Single(Pins(), p => p.Name == name);

    /// <summary>Banners as the tools actually print them, captured 2026-10-16.</summary>
    private static readonly (string Tool, string Banner, string Expected)[] Banners =
    {
        ("clang",  "clang version 23.1.2 (https://github.com/llvm/llvm-project 85ac560262434c9ccfc0c183ec22d4138ed647fb)", "23.1.2"),
        ("clang",  "Ubuntu clang version 23.1.2 (++20260901083512+85ac56026243-1~exp1~20260901083632.12)", "23.1.2"),
        ("ld.lld", "LLD 23.1.2 (https://github.com/llvm/llvm-project 85ac560262434c9ccfc0c183ec22d4138ed647fb) (compatible with GNU linkers)", "23.1.2"),
        ("nasm",   "NASM version 3.02 compiled on Jun 28 2026", "3.02.0"),
        ("cmake",  "cmake version 4.4.3", "4.4.3"),
        ("ninja",  "1.13.2", "1.13.2"),
    };

    public static TheoryData<string, string, string> RealBanners
    {
        get
        {
            var data = new TheoryData<string, string, string>();
            foreach (var (tool, banner, expected) in Banners) data.Add(tool, banner, expected);
            return data;
        }
    }

    [Theory]
    [MemberData(nameof(RealBanners))]
    [Trait(Categories.Key, Categories.Unit)]
    public void Each_pin_reads_its_tools_real_banner(string tool, string banner, string expected)
    {
        Assert.Equal(Version.Parse(expected), ToolchainPolicy.ReadVersion(Pin(tool), banner));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_pinned_tool_has_a_real_banner_case()
    {
        // A tool added to versions.json without a banner here has a pattern
        // nobody has checked against anything.
        var covered = Banners.Select(b => b.Tool).ToHashSet();
        Assert.All(Pins(), p => Assert.Contains(p.Name, covered));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_clang_below_the_floor_is_refused_and_one_at_it_is_not()
    {
        var clang = Pin("clang");
        ToolFinding Find(string banner) => new(clang, banner, ToolchainPolicy.ReadVersion(clang, banner));

        // Both sides of the bound. 22 is the newest clang known to link through gcc.
        Assert.True(Find("clang version 22.1.8 (https://github.com/llvm/llvm-project abc)").BelowMinimum);
        Assert.False(Find("clang version 23.0.0 (https://github.com/llvm/llvm-project abc)").BelowMinimum);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_unreadable_banner_is_refused_when_a_floor_exists_and_allowed_when_none_does()
    {
        // "Could not tell" is not evidence a tool is new enough. Where no floor is
        // known there is nothing to enforce, so the same banner is fine.
        const string garbage = "some wrapper script v?";

        var clang = Pin("clang");
        Assert.True(new ToolFinding(clang, garbage, ToolchainPolicy.ReadVersion(clang, garbage)).BelowMinimum);

        var nasm = Pin("nasm");
        Assert.Null(nasm.Minimum);
        Assert.False(new ToolFinding(nasm, garbage, ToolchainPolicy.ReadVersion(nasm, garbage)).BelowMinimum);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_missing_tool_is_not_reported_as_too_old()
    {
        // Missing and too old are separate refusals with separate messages.
        var f = new ToolFinding(Pin("clang"), null, null);
        Assert.False(f.Found);
        Assert.False(f.BelowMinimum);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_ci_image_installs_the_pinned_llvm()
    {
        string dockerfile = File.ReadAllText(Path.Combine(Requires.Repo(), "tools", "ci", "Dockerfile"));
        var m = Regex.Match(dockerfile, @"^ARG LLVM_VERSION=([0-9]+)\s*$", RegexOptions.Multiline);
        Assert.True(m.Success, "tools/ci/Dockerfile no longer declares ARG LLVM_VERSION=<major>.");

        int llvm = int.Parse(m.Groups[1].Value);
        Assert.Equal(llvm, Pin("clang").Minimum!.Major);
        Assert.Equal(llvm, Pin("ld.lld").Minimum!.Major);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void CMake_and_the_pin_agree_on_cmakes_minimum()
    {
        string cmake = File.ReadAllText(Path.Combine(Requires.Repo(), "tools", "cmake", "CMakeLists.txt"));
        var m = Regex.Match(cmake, @"cmake_minimum_required\(VERSION ([0-9.]+)\)");
        Assert.True(m.Success, "CMakeLists.txt has no cmake_minimum_required(VERSION ...).");

        Assert.Equal(ToolchainPolicy.ParseVersion(m.Groups[1].Value), Pin("cmake").Minimum);
    }
}
