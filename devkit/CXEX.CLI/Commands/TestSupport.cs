// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;

namespace CXEX.CLI.Commands;

/// <summary>
/// What <c>cxk test all</c> and <c>cxk test sabotage</c> share: running this
/// cxk's own <c>os build</c> and <c>verify-boot</c> as child processes (so a
/// build's 300 lines go to a log, not the console), and the checks that make
/// a build's result trustworthy.
/// </summary>
internal static class TestSupport
{
    public static string Image(string root) => Path.Combine(root, "dist", "CXK_x86_64", "images", "cxk_x86_64.img");

    /// <summary>Runs this cxk with <paramref name="args"/>, its output to <paramref name="log"/>.</summary>
    public static ToolResult Self(string root, IEnumerable<string> args, string log, TimeSpan timeout)
    {
        string exe = Environment.ProcessPath ?? "cxk";
        var list = new List<string>();
        // under `dotnet CXEX.CLI.dll` the process is dotnet, not cxk
        if (Path.GetFileNameWithoutExtension(exe).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
            list.Add(typeof(TestSupport).Assembly.Location);
        list.AddRange(args);
        var r = ToolProcess.Run(exe, list, new ToolRunOptions { WorkingDirectory = root, Timeout = timeout });
        File.WriteAllText(log, $"$ {r.CommandLine()}\n{r.StandardOutput}\n--- stderr ---\n{r.StandardError}\n--- {r.Describe()}\n");
        return r;
    }

    /// <summary>
    /// <c>cxk os build</c>, then a check that it wrote a new image (and a new
    /// kernel, when <paramref name="kernelChanged"/>). Null when it did;
    /// otherwise why not. A failed or stale build is never a test's verdict:
    /// the boot that would follow would test the last image, not this tree.
    /// </summary>
    public static string? Build(string root, IReadOnlyList<string> buildArgs, string log, bool kernelChanged)
    {
        DateTime stamp = DateTime.UtcNow.AddSeconds(-1);   // a file system's mtime is coarse
        var r = Self(root, ["os", "build", .. buildArgs], log, TimeSpan.FromMinutes(30));
        if (!r.Ok)
        {
            string first = (r.StandardOutput + "\n" + r.StandardError).Split('\n')
                .FirstOrDefault(l => l.Contains("error", StringComparison.OrdinalIgnoreCase))?.Trim() ?? r.Describe();
            return $"the build failed: {first} (log: {log})";
        }
        if (!File.Exists(Image(root)) || File.GetLastWriteTimeUtc(Image(root)) < stamp)
            return $"the build left an old image (log: {log})";
        string kernel = Path.Combine(root, "build", "kernel.elf");
        if (kernelChanged && (!File.Exists(kernel) || File.GetLastWriteTimeUtc(kernel) < stamp))
            return $"the build left an old build/kernel.elf after a kernel edit (log: {log})";
        return null;
    }

    /// <summary>
    /// <c>cxk verify-boot --full --json</c> on the built image, the serial log
    /// and the JSON kept in <paramref name="dir"/>. The parsed JSON, or null
    /// and why (verify-boot could not run, or printed no JSON).
    /// </summary>
    public static (BootJson? Boot, string? Problem) Boot(string root, IReadOnlyList<string> extra, string dir, int timeoutSeconds)
    {
        Directory.CreateDirectory(dir);
        string serial = Path.Combine(dir, "serial.log");
        var r = Self(root, ["verify-boot", Image(root), "--full", "-t", timeoutSeconds.ToString(System.Globalization.CultureInfo.InvariantCulture),
                            "--json", "--log", serial, .. extra],
                     Path.Combine(dir, "verify-boot.log"), TimeSpan.FromSeconds(timeoutSeconds + 300));
        File.WriteAllText(Path.Combine(dir, "boot.json"), r.StandardOutput);
        if (r.Outcome is ToolOutcome.NotFound or ToolOutcome.TimedOut) return (null, r.Describe());
        var b = BootJson.Parse(r.StandardOutput);
        return b is null ? (null, $"verify-boot printed no JSON (exit {r.ExitCode}): {r.StandardError.Trim()}") : (b, null);
    }
}
