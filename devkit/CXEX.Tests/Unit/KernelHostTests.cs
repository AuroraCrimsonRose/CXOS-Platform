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
        string repo = Requires.Repo();
        var missing = new[] { "clang", "cmake", "ninja" }.Where(t => !TestEnv.HasTool(t)).ToList();
        if (missing.Count > 0)
            Assert.Skip($"missing from PATH: {string.Join(", ", missing)}. The kernel host tests build with " +
                        "clang, CMake and Ninja (kernel/test/host/CMakeLists.txt).");

        string src = Path.Combine(repo, "kernel", "test", "host");
        string bin = Path.Combine(repo, "build", "hosttest");

        var cfg = Run("cmake", new[] { "-S", src, "-B", bin, "-G", "Ninja", "-DCMAKE_C_COMPILER=clang" }, repo, null);
        Assert.True(cfg.Code == 0, $"configure failed:\n{cfg.Out}\n{cfg.Err}");

        var build = Run("ninja", new[] { "-C", bin }, repo, null);
        Assert.True(build.Code == 0, $"build failed:\n{build.Out}\n{build.Err}");

        string exe = Path.Combine(bin, OperatingSystem.IsWindows() ? "khost_tests.exe" : "khost_tests");
        Assert.True(File.Exists(exe), $"the build reported success but {exe} does not exist");

        // On Windows the sanitizer runtime is a DLL beside clang's resource
        // directory, not on the default search path.
        string? extraPath = null;
        if (OperatingSystem.IsWindows())
        {
            var rd = Run("clang", new[] { "-print-resource-dir" }, repo, null);
            Assert.True(rd.Code == 0, $"clang -print-resource-dir failed: {rd.Err}");
            extraPath = Path.Combine(rd.Out.Trim(), "lib", "windows");
        }

        var run = Run(exe, Array.Empty<string>(), repo, extraPath);
        Assert.True(run.Code == 0 && run.Out.Contains("host tests: all ", StringComparison.Ordinal),
            $"kernel host tests failed (exit {run.Code}):\n{run.Out}\n{run.Err}");
    }

    private sealed record Result(int Code, string Out, string Err);

    private static Result Run(string exe, IEnumerable<string> args, string cwd, string? prependPath)
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
