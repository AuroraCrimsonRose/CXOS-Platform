using Xunit;

namespace CXEX.Tests;

/// <summary>
/// Preconditions, expressed as skips that carry their reason.
///
/// <para>This is the whole point of D1's rule that a test whose requirement is
/// missing "reports skipped, with the reason — it never silently passes". A
/// differential test that quietly returns when clang is absent is worse than no
/// test: the run is green and nothing was compared. Every one of these calls
/// ends the test with a sentence naming exactly what was missing.</para>
/// </summary>
public static class Requires
{
    /// <summary>A checkout — anything reading the OS sources or the test corpus.</summary>
    public static string Repo()
    {
        if (TestEnv.RepoRoot is null)
            Assert.Skip("no CXOS checkout found: abi/cxk_abi.h and tools/cmake/CMakeLists.txt were not found " +
                        "above the test binary. Set CXK_ROOT to the repository root.");
        return TestEnv.RepoRoot!;
    }

    /// <summary>A built `cxk`, which every compile-and-compare test drives.</summary>
    public static string Cxk()
    {
        Repo();
        if (TestEnv.Cxk is null)
            Assert.Skip("no `cxk` found: looked at $CXK, tools/cxk.exe and devkit/CXEX.CLI/bin/Release/net10.0. " +
                        "Build it with: dotnet publish devkit/CXEX.CLI -c Release");
        return TestEnv.Cxk!;
    }

    /// <summary>
    /// The LLVM toolchain (D5). Named separately from <see cref="Linux"/> because a
    /// Windows host can have clang and still not be able to run the output.
    /// </summary>
    public static void Toolchain()
    {
        var missing = new List<string>();
        if (!TestEnv.HasTool("clang")) missing.Add("clang");
        if (!TestEnv.HasTool("ld.lld")) missing.Add("ld.lld");
        if (missing.Count > 0)
            Assert.Skip($"missing from PATH: {string.Join(", ", missing)}. " +
                        "The LLVM toolchain compiles and links the X output (HARDENING_PLAN D5).");
    }

    /// <summary>
    /// A host that can execute the compiled X directly. X's host platform makes
    /// Linux system calls, so a 32-bit ELF it produces will not run on Windows or
    /// macOS — those hosts skip rather than fail.
    /// </summary>
    public static void Linux()
    {
        if (!OperatingSystem.IsLinux())
            Assert.Skip($"needs a Linux host or WSL: compiled X makes Linux system calls directly, " +
                        $"so it cannot run on {(OperatingSystem.IsWindows() ? "Windows" : "this host")}.");
    }
}
