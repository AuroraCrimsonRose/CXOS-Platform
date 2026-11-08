// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.Text;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// Kernel C, tested on the host (kernel/test/host, docs/safety/CX_SAFETY.md
/// §3.7). This builds the host test executable from the tree's own kernel
/// sources with clang, AddressSanitizer and UndefinedBehaviorSanitizer, runs
/// it, and requires its summary line.
///
/// <para>It is here so that `dotnet test` stays the one command for host tests.
/// A missing toolchain skips with the reason; it never passes silently.</para>
/// </summary>
public class KernelHostTests
{
    [Fact]
    [Trait(Categories.Key, Categories.Toolchain)]
    public void Kernel_code_passes_its_host_tests_under_the_sanitizers()
    {
        var (repo, bin, extraPath) = Build();
        string exe = Path.Combine(bin, OperatingSystem.IsWindows() ? "khost_tests.exe" : "khost_tests");
        Assert.True(File.Exists(exe), $"the build reported success but {exe} does not exist");

        var run = Run(exe, Array.Empty<string>(), repo, extraPath);
        Assert.True(run.Code == 0 && run.Out.Contains("host tests: all ", StringComparison.Ordinal),
            $"kernel host tests failed (exit {run.Code}):\n{run.Out}\n{run.Err}");
    }

    /// <summary>
    /// The CXFS fuzz target (kernel/test/host/fuzz_cxfs.c, CX_SAFETY.md §3.2)
    /// for a fixed number of inputs from a fixed seed, starting from the
    /// tree's corpus. libFuzzer with one seed and one corpus makes the same
    /// inputs on every run, so a red here is a finding to reproduce, not
    /// noise. New inputs go to a scratch directory, never the tree. A longer
    /// hunt is the command in fuzz_cxfs.c.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Toolchain)]
    public void The_cxfs_fuzz_target_holds_for_its_fixed_run()
    {
        var (repo, bin, extraPath) = Build();
        string missing = Path.Combine(bin, "cxfs_fuzz_missing.txt");
        if (File.Exists(missing))
            Assert.Skip("the CXFS fuzz target was not built: " + File.ReadAllText(missing).Trim());
        string exe = Path.Combine(bin, OperatingSystem.IsWindows() ? "cxfs_fuzz.exe" : "cxfs_fuzz");
        Assert.True(File.Exists(exe), $"the build reported success but {exe} does not exist");

        string scratch = Path.Combine(bin, "fuzz-cxfs-fixed");
        if (Directory.Exists(scratch)) Directory.Delete(scratch, recursive: true);
        Directory.CreateDirectory(scratch);
        string corpus = Path.Combine(repo, "kernel", "test", "host", "corpus", "cxfs");

        var run = Run(exe, new[] { "-seed=1", "-runs=" + FuzzRuns, "-max_len=65536", "-timeout=10",
                                   "-artifact_prefix=" + scratch + Path.DirectorySeparatorChar, scratch, corpus },
                      repo, extraPath);
        Assert.True(run.Code == 0 && run.Err.Contains("Done " + FuzzRuns + " runs", StringComparison.Ordinal),
            $"the CXFS fuzz target failed (exit {run.Code}):\n{Tail(run.Err)}");
    }

    /// <summary>Inputs in the fixed run (CX_SAFETY.md §3.2 has what it costs).</summary>
    private const int FuzzRuns = 20000;

    private static string Tail(string s) => s.Length <= 4000 ? s : s[^4000..];

    private static readonly object BuildLock = new();

    /// <summary>Configures and builds kernel/test/host, skipping with the reason
    /// when the toolchain is missing. Returns the repository, the build
    /// directory, and what PATH needs for the sanitizer runtime.</summary>
    internal static (string Repo, string Bin, string? ExtraPath) Build()
    {
        string repo = Requires.Repo();
        var missing = new[] { "clang", "cmake", "ninja" }.Where(t => !TestEnv.HasTool(t)).ToList();
        if (missing.Count > 0)
            Assert.Skip($"missing from PATH: {string.Join(", ", missing)}. The kernel host tests build with " +
                        "clang, CMake and Ninja (kernel/test/host/CMakeLists.txt).");

        string src = Path.Combine(repo, "kernel", "test", "host");
        string bin = Path.Combine(repo, "build", "hosttest");

        lock (BuildLock)
        {
            var cfg = Run("cmake", new[] { "-S", src, "-B", bin, "-G", "Ninja", "-DCMAKE_C_COMPILER=clang" }, repo, null);
            Assert.True(cfg.Code == 0, $"configure failed:\n{cfg.Out}\n{cfg.Err}");

            var build = Run("ninja", new[] { "-C", bin }, repo, null);
            Assert.True(build.Code == 0, $"build failed:\n{build.Out}\n{build.Err}");
        }

        // On Windows the sanitizer runtime is a DLL beside clang's resource
        // directory, not on the default search path.
        string? extraPath = null;
        if (OperatingSystem.IsWindows())
        {
            var rd = Run("clang", new[] { "-print-resource-dir" }, repo, null);
            Assert.True(rd.Code == 0, $"clang -print-resource-dir failed: {rd.Err}");
            extraPath = Path.Combine(rd.Out.Trim(), "lib", "windows");
        }
        return (repo, bin, extraPath);
    }

    internal sealed record Result(int Code, string Out, string Err);

    internal static Result Run(string exe, IEnumerable<string> args, string cwd, string? prependPath)
    {
        var psi = new ProcessStartInfo
        {
            FileName = exe,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = cwd,
        };
        foreach (string a in args) psi.ArgumentList.Add(a);
        if (prependPath is not null)
            psi.Environment["PATH"] = prependPath + Path.PathSeparator + Environment.GetEnvironmentVariable("PATH");

        using var p = Process.Start(psi) ?? throw new InvalidOperationException($"could not start {exe}");
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) stdout.AppendLine(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) stderr.AppendLine(e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        if (!p.WaitForExit(600_000))
        {
            try { p.Kill(entireProcessTree: true); } catch { /* already gone */ }
            throw new TimeoutException($"{exe} did not finish within 10 minutes");
        }
        p.WaitForExit();
        return new Result(p.ExitCode, stdout.ToString(), stderr.ToString());
    }
}
