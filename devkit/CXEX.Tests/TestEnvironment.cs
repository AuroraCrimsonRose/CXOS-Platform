using System.Diagnostics;

namespace CXEX.Tests;

/// <summary>
/// What the suite can see of the machine it is running on: where the repository
/// is, which external tools exist, and the knobs that size a run.
///
/// <para>Everything here is discovered once and cached. Nothing in this class
/// throws for a missing tool — that is <see cref="Requires"/>'s job, because a
/// missing requirement must surface as a <em>skip carrying its reason</em>, never
/// as a failure and never as a silent pass (D1).</para>
/// </summary>
public static class TestEnv
{
    /// <summary>
    /// Mutant count for the differential suites. The default is small enough that
    /// every local run can afford it; a nightly or pre-release run sets thousands
    /// through <c>CXEX_TEST_MUTANTS</c>.
    /// </summary>
    public static int Mutants => ReadInt("CXEX_TEST_MUTANTS", 50);

    /// <summary>
    /// Seed for every generator in the suite, so a failing run is reproducible by
    /// quoting one number. Set <c>CXEX_TEST_SEED</c> to replay one.
    /// </summary>
    public static int Seed => ReadInt("CXEX_TEST_SEED", 1);

    /// <summary>
    /// True when the harness should plant a difference in each comparison to prove
    /// it can still see one. Each harness has a unit test that sets this and
    /// requires a failure, so a comparison can never quietly compare nothing (D1).
    /// </summary>
    public static bool Sabotage => Environment.GetEnvironmentVariable("SABOTAGE") == "1";

    private static readonly Lazy<string?> _root = new(FindRepoRoot);

    /// <summary>The repository root, or null if this is running outside a checkout.</summary>
    public static string? RepoRoot => _root.Value;

    /// <summary>Test data lives in <c>devkit/tests/</c>; only the scripts were retired, not the corpus.</summary>
    public static string? TestDataDir => RepoRoot is null ? null : Path.Combine(RepoRoot, "devkit", "tests");

    /// <summary>The ABI header both sides compile against.</summary>
    public static string? AbiHeader => RepoRoot is null ? null : Path.Combine(RepoRoot, "abi", "cxk_abi.h");

    /// <summary>
    /// The `cxk` CLI, if a built one can be found. Checked in the order a developer
    /// would expect: an explicit CXK override, the copy the OS build uses, then the
    /// Release output of CXEX.CLI.
    /// </summary>
    public static string? Cxk
    {
        get
        {
            string? explicitPath = Environment.GetEnvironmentVariable("CXK");
            if (explicitPath is { Length: > 0 } && File.Exists(explicitPath)) return explicitPath;
            if (RepoRoot is null) return null;

            string exe = OperatingSystem.IsWindows() ? "cxk.exe" : "cxk";
            foreach (string candidate in new[]
                     {
                         Path.Combine(RepoRoot, "tools", exe),
                         Path.Combine(RepoRoot, "devkit", "CXEX.CLI", "bin", "Release", "net10.0", exe),
                     })
            {
                if (File.Exists(candidate)) return candidate;
            }
            return null;
        }
    }

    /// <summary>True when <paramref name="exe"/> can be started at all.</summary>
    public static bool HasTool(string exe) => _tools.GetOrAdd(exe, Probe);

    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, bool> _tools = new();

    private static bool Probe(string exe)
    {
        // ld.lld carries a dot, so Windows CreateProcess will not append .exe for it;
        // try the explicit name too rather than call a present linker missing.
        foreach (string name in OperatingSystem.IsWindows() ? new[] { exe, exe + ".exe" } : new[] { exe })
        {
            try
            {
                using var p = Process.Start(new ProcessStartInfo
                {
                    FileName = name,
                    Arguments = "--version",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                });
                if (p is null) continue;
                p.WaitForExit(15_000);
                return true;
            }
            catch
            {
                // not on PATH under this name - try the next spelling
            }
        }
        return false;
    }

    private static int ReadInt(string name, int fallback) =>
        int.TryParse(Environment.GetEnvironmentVariable(name), out int v) && v > 0 ? v : fallback;

    /// <summary>
    /// Walks up for the two files that together identify this repository. Both,
    /// not either: a lone <c>tools/</c> directory matches too many trees.
    /// <c>CXK_ROOT</c> still overrides, as it did for the Python suites.
    /// </summary>
    private static string? FindRepoRoot()
    {
        string? env = Environment.GetEnvironmentVariable("CXK_ROOT");
        if (env is { Length: > 0 } && IsRoot(env)) return Path.GetFullPath(env);

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (IsRoot(dir.FullName)) return dir.FullName;
            dir = dir.Parent;
        }
        return null;
    }

    private static bool IsRoot(string dir) =>
        File.Exists(Path.Combine(dir, "abi", "cxk_abi.h")) &&
        File.Exists(Path.Combine(dir, "tools", "cmake", "CMakeLists.txt"));
}
