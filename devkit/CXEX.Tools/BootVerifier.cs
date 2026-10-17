// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.Text.RegularExpressions;

namespace CXEX.Tools;

public enum BootOutcome
{
    /// <summary>The self-tests reported all passed (and, for a full boot, the executive started).</summary>
    Pass = 0,
    /// <summary>A panic, or the self-tests reported a failure.</summary>
    Fail = 1,
    /// <summary>Neither, before the time ran out. Never read as a pass.</summary>
    Inconclusive = 2,
}

/// <summary>What a boot log says, and the line that says it.</summary>
public sealed record BootVerdict(BootOutcome Outcome, string Reason, int? TestsPassed)
{
    /// <summary>The kernel's own summary line.</summary>
    private static readonly Regex Summary = new(@"self-tests?:\s*all\s+(\d+)\s+passed", RegexOptions.IgnoreCase);
    private static readonly Regex TestsFailed = new(@"self-tests failed", RegexOptions.IgnoreCase);

    /// <summary>
    /// The kernel's panic banner. Matched exactly, not as the word "panic": the
    /// self-tests drive refusal paths on purpose and log about them, and the
    /// old VGA harness had to keep a list of lines that only looked alarming.
    /// The banner is the one thing both panic paths (CPU exception, kpanic)
    /// always print, and the panic path writes to serial too.
    /// </summary>
    private static readonly Regex Panic = new(@"\*\*\* KERNEL PANIC \*\*\*");
    private static readonly Regex ExecutiveUp = new(@"executive started", RegexOptions.IgnoreCase);

    /// <summary>
    /// Reads a serial log. A panic beats everything, including a summary
    /// printed before it: the verdict is about the whole log, so a later panic
    /// is not excused by an earlier pass. <paramref name="full"/> also requires
    /// the executive to have started, which the self-test summary precedes.
    /// </summary>
    public static BootVerdict Classify(string log, bool full)
    {
        var panic = Panic.Match(log);
        if (panic.Success)
        {
            int end = log.IndexOf('\n', panic.Index);
            string tail = log.Substring(panic.Index, (end < 0 ? log.Length : Math.Min(log.Length, end + 200)) - panic.Index);
            return new BootVerdict(BootOutcome.Fail, "kernel panic: " + tail.Replace("\r", "").Trim(), null);
        }
        if (TestsFailed.IsMatch(log))
            return new BootVerdict(BootOutcome.Fail, "the self-tests reported a failure", null);

        var s = Summary.Match(log);
        if (!s.Success)
            return new BootVerdict(BootOutcome.Inconclusive, "no self-test summary yet", null);

        int n = int.Parse(s.Groups[1].Value);
        if (full && !ExecutiveUp.IsMatch(log))
            return new BootVerdict(BootOutcome.Inconclusive, $"self-tests: all {n} passed, but the executive has not started", n);

        return new BootVerdict(BootOutcome.Pass, full
            ? $"self-tests: all {n} passed, and the executive started"
            : $"self-tests: all {n} passed", n);
    }
}

/// <summary>
/// Boots an image headless and reads the verdict off the serial console - the
/// gate a kernel change has to pass (CLAUDE.md), as a cxk command so it is the
/// same on Windows and in the Linux CI image (HARDENING_PLAN D2). It replaces
/// two scripts that lived outside the repository: serialboot.ps1 (Windows only)
/// and bootverify.py (which decoded the VGA text buffer, and so was blind in
/// the default framebuffer mode).
/// </summary>
public static class BootVerifier
{
    /// <summary>
    /// The configuration every verification boot uses: the pc machine (q35 makes
    /// the ATA self-test vacuous), -snapshot (CXOS writes to its disk on first
    /// boot, and verifying a release image without it has broken its
    /// SHA256SUMS), and -icount for timing that does not depend on the host.
    /// </summary>
    public static QemuConfig ConfigFor(string image, string serialLog, string? cpu = null) => new()
    {
        BootDisk = image,
        MachineType = "pc",
        MemoryMb = 256,
        EnableAudio = false,
        EnableNetworking = false,
        Snapshot = true,
        Headless = true,
        Serial = serialLog,
        ICountShift = "6",
        CpuModel = cpu,
    };

    /// <summary>
    /// Boots, polls the log, and stops as soon as the verdict is decided or the
    /// time is up. Kills only the QEMU it started: killing by process name
    /// would take down any other emulator on the machine.
    /// </summary>
    public static BootVerdict Run(string image, string serialLog, TimeSpan timeout, bool full, Action<string>? progress = null, string? cpu = null)
    {
        if (File.Exists(serialLog)) File.Delete(serialLog);

        var psi = new ProcessStartInfo
        {
            FileName = ExecutableResolver.Resolve(QemuTool.Executable),
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (string a in QemuTool.BuildArguments(ConfigFor(image, serialLog, cpu))) psi.ArgumentList.Add(a);

        using var qemu = Process.Start(psi)
            ?? throw new InvalidOperationException($"could not start {QemuTool.Executable}");
        qemu.OutputDataReceived += (_, _) => { };
        qemu.ErrorDataReceived += (_, e) => { if (e.Data is { Length: > 0 }) progress?.Invoke("qemu: " + e.Data); };
        qemu.BeginOutputReadLine();
        qemu.BeginErrorReadLine();

        var clock = Stopwatch.StartNew();
        BootVerdict verdict = new(BootOutcome.Inconclusive, "QEMU produced no serial output", null);
        try
        {
            while (clock.Elapsed < timeout)
            {
                Thread.Sleep(500);
                string log = ReadShared(serialLog);
                verdict = BootVerdict.Classify(log, full);
                if (verdict.Outcome != BootOutcome.Inconclusive) break;
                if (qemu.HasExited)
                {
                    verdict = verdict with { Reason = $"QEMU exited (code {qemu.ExitCode}) before a verdict: {verdict.Reason}" };
                    break;
                }
            }
        }
        finally
        {
            if (!qemu.HasExited)
            {
                try { qemu.Kill(entireProcessTree: true); } catch (InvalidOperationException) { /* already gone */ }
                qemu.WaitForExit(10_000);
            }
        }

        if (verdict.Outcome == BootOutcome.Inconclusive && clock.Elapsed >= timeout)
            verdict = verdict with { Reason = $"no verdict within {timeout.TotalSeconds:0} s: {verdict.Reason}" };
        return verdict;
    }

    /// <summary>QEMU holds the log open for writing; read it without fighting for the handle.</summary>
    private static string ReadShared(string path)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var r = new StreamReader(fs);
            return r.ReadToEnd();
        }
        catch (FileNotFoundException) { return ""; }
        catch (IOException) { return ""; }
    }
}
