// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.Text.RegularExpressions;

namespace CXEX.Tools;

public enum BootOutcome
{
    /// <summary>The self-tests reported all passed (and, for a full boot, the executive and the shell ran).</summary>
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
    // spawn.c, when the executive's image is refused before ring 3. "Started" is
    // logged only once it is loaded, so without this a refused executive would
    // read as "not started yet" until the timeout.
    private static readonly Regex ExecutiveRefused = new(@"executive refused at load", RegexOptions.IgnoreCase);
    // "Started" is the kernel's word that the image loaded; these two are
    // written from ring 3. The banner is executive.c's first syscall, the
    // prompt the shell's (an X program the executive spawned). A broken
    // syscall wrapper lost the banner while "started" stayed (2026-10-28).
    private static readonly Regex ExecutiveBanner = new(@"CXK executive online");
    private static readonly Regex ShellPrompt = new(@"cxk> ");
    // usermode.c's grant gates: "<what> DENIED for pid N (no GRANT_X)". The
    // executive and what it starts hold what they use, so an idle boot logs
    // none after "executive started". A shell built against an old baseline
    // printed its prompt, was refused the keyboard and prompted again, for
    // good, and the prompt alone passed (1.7a).
    private static readonly Regex GrantDenied = new(@"(\w+) DENIED for pid (\d+)");

    /// <summary>
    /// Reads a serial log. A panic beats everything, including a summary
    /// printed before it: the verdict is about the whole log, so a later panic
    /// is not excused by an earlier pass. <paramref name="full"/> also requires
    /// the executive to have started, then its banner and the shell's prompt,
    /// all of which the self-test summary precedes. With
    /// <paramref name="results"/> (every x86-64 kernel since 1.5d), a summary
    /// also needs the per-test <c>@ktest</c> lines behind it, agreeing with it
    /// (<see cref="SelfTestReport"/>): a summary alone fails.
    /// </summary>
    public static BootVerdict Classify(string log, bool full, bool results = true, IReadOnlySet<string>? notJudged = null)
    {
        var panic = Panic.Match(log);
        if (panic.Success)
        {
            int end = log.IndexOf('\n', panic.Index);
            string tail = log.Substring(panic.Index, (end < 0 ? log.Length : Math.Min(log.Length, end + 200)) - panic.Index);
            return new BootVerdict(BootOutcome.Fail, "kernel panic: " + tail.Replace("\r", "").Trim(), null);
        }
        int n;
        string head;
        if (TestsFailed.IsMatch(log))
        {
            var report = results ? SelfTestReport.Parse(log) : null;
            var failed = report?.Results.Where(r => !r.Passed).Select(r => r.Name).ToList() ?? [];
            var judged = failed.Where(f => notJudged is null || !notJudged.Contains(f)).ToList();
            // Only failures of tests this boot does not judge, and the end
            // line agreeing with the results: judged as if they had passed,
            // and named in the verdict, never silently (the stress boot).
            if (failed.Count == 0 || judged.Count > 0 || report!.EndTotal is not int total
                || report.EndPassed != total - failed.Count || report.Results.Count != total)
                return new BootVerdict(BootOutcome.Fail, judged.Count == 0 && failed.Count == 0
                    ? "the self-tests reported a failure"
                    : "the self-tests reported a failure: " + string.Join("; ", judged.Count > 0 ? judged : failed), null);
            n = total - failed.Count;
            head = $"self-tests: {total - failed.Count} of {total} passed, the rest not judged here ({string.Join("; ", failed)})";
        }
        else
        {
            var s = Summary.Match(log);
            if (!s.Success)
                return new BootVerdict(BootOutcome.Inconclusive, "no self-test summary yet", null);

            n = int.Parse(s.Groups[1].Value);
            head = $"self-tests: all {n} passed";
            if (results && SelfTestReport.Parse(log).DisagreesWith(n) is string wrong)
                return new BootVerdict(BootOutcome.Fail, $"{head}, but {wrong}", n);
        }
        // The hardware report precedes the self-tests (kmain), so a summary
        // without a whole one behind it means the serial copy was lost or torn.
        if (results && HwProblem(log) is string hw)
            return new BootVerdict(BootOutcome.Fail, $"{head}, but {hw}", n);
        if (full && ExecutiveRefused.IsMatch(log))
            return new BootVerdict(BootOutcome.Fail, $"{head}, but the executive was refused at load", n);
        if (full && !ExecutiveUp.IsMatch(log))
            return new BootVerdict(BootOutcome.Inconclusive, $"{head}, but the executive has not started", n);
        if (full && !ExecutiveBanner.IsMatch(log))
            return new BootVerdict(BootOutcome.Inconclusive, $"{head}, the executive loaded, but its banner has not appeared", n);
        if (full && GrantDenied.Match(log, ExecutiveUp.Match(log).Index) is { Success: true } denied)
            return new BootVerdict(BootOutcome.Fail, $"{head}, but after the executive started, {denied.Groups[1].Value} was refused to pid {denied.Groups[2].Value}", n);
        if (full && !ShellPrompt.IsMatch(log))
            return new BootVerdict(BootOutcome.Inconclusive, $"{head}, the executive ran, but the shell's prompt has not appeared", n);

        return new BootVerdict(BootOutcome.Pass, full
            ? $"{head}, and the executive and the shell ran"
            : $"{head}", n);
    }

    /// <summary>Null when the log holds a whole hardware report (<see cref="HwReport"/>); otherwise what is wrong.</summary>
    private static string? HwProblem(string log)
    {
        string? text = HwReport.TextFromSerial(log);
        if (text is null) return "the hardware report is missing";
        return HwReport.Check(text) is string wrong ? "the hardware report is damaged: " + wrong : null;
    }

    /// <summary>The kpanic reason a crash-log test request leads to (kernel/kmain.c).</summary>
    public const string CrashTestReason = "deliberate panic: a crash-log test asked for one";

    /// <summary>
    /// The first boot of <c>cxk verify-crashlog</c>, which is asked to panic: it
    /// passes when it panicked for that reason and said it saved the crash log,
    /// and fails when it said it could not, panicked for another reason, or got
    /// past the self-tests without panicking. The console wraps at its width,
    /// so lines are joined before they are searched.
    /// </summary>
    public static BootVerdict ClassifyRequestedPanic(string log)
    {
        string joined = log.Replace("\r", "").Replace("\n", "");
        bool panicked = Panic.IsMatch(joined);
        if (panicked && joined.Contains("Crash log saved"))
            return joined.Contains(CrashTestReason)
                ? new BootVerdict(BootOutcome.Pass, "panicked on request and saved the crash log", null)
                : new BootVerdict(BootOutcome.Fail, "panicked, but not the requested panic", null);
        if (panicked && joined.Contains("Crash log not saved"))
            return new BootVerdict(BootOutcome.Fail, "panicked, but the crash log was not saved", null);
        if (panicked && joined.Contains("System halted"))
            return new BootVerdict(BootOutcome.Fail, "panicked without saying whether the crash log was saved", null);
        if (ExecutiveUp.IsMatch(joined) || TestsFailed.IsMatch(joined))
            return new BootVerdict(BootOutcome.Fail, "the requested panic did not happen", null);
        return new BootVerdict(BootOutcome.Inconclusive, "no panic yet", null);
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
    /// The x86-64 kernel gets 5 GB, so RAM lies on both sides of 4 GB and its
    /// page allocator's two zones, and the hole below 4 GB, are all exercised
    /// (docs/kernel/CX_KERNEL_PLAN.md section 9.3).
    /// </summary>
    /// <summary>The x86-64 verification boot's RAM: past 4 GB.</summary>
    public const int X86_64MemoryMb = 5120;

    /// <summary>The x86-64 verification boot's CPUs (1.9d): four, by a
    /// topology whose APIC IDs are 0, 1, 2 and 4 (three cores take two bits
    /// of ID), with two more possible CPUs the MADT lists disabled, as the
    /// rig lists sixteen. So every boot starts the APs by the IDs the MADT
    /// gives, not by their count or order, and leaves the disabled ones off
    /// (docs/kernel/CX_SMP.md). <c>--smp 1</c> boots one.</summary>
    public const string X86_64Smp = "4,maxcpus=6,sockets=2,cores=3,threads=1";

    /// <summary>The stress boot's CPUs (1.9f): eight, as the rig has, each a
    /// host thread under multi-threaded TCG and with no -icount, which would
    /// run them one at a time. Its timing is the host's, so it decides
    /// nothing about cost (docs/kernel/CX_SMP.md §4a); it is there for
    /// orderings one CPU at a time never makes.</summary>
    public const string X86_64StressSmp = "8";
    public const string StressAccel = "tcg,thread=multi";

    /// <summary>The self-tests a stress boot does not judge: each bounds a time
    /// by the virtual clock of <c>-icount</c>, which a stress boot has not, so
    /// against the host's clock it measures the host (seen 2026-11-07: the
    /// deadline spread and the slack window miss by the host timer's grain).
    /// They still run,
    /// are named in the verdict when they fail, and are judged by every other
    /// boot.</summary>
    public static readonly IReadOnlySet<string> StressNotJudged = new HashSet<string>
    {
        "deadlines are met at their time, not at a tick",
        "slack: a timer fires at a later one's interrupt",
    };

    public static QemuConfig ConfigFor(string image, string serialLog, string? cpu = null, bool snapshot = true, UsbHost? usb = null, string? foreignDisk = null, QemuArch arch = QemuArch.X86_64, string? smp = null, bool stress = false) => new()
    {
        BootDisk = image,
        MachineType = "pc",
        MemoryMb = arch == QemuArch.X86_64 ? X86_64MemoryMb : 256,
        EnableAudio = false,
        EnableNetworking = false,
        Snapshot = snapshot,
        Headless = true,
        Serial = serialLog,
        ICountShift = stress ? null : "6",
        ICountNoSleep = !stress,
        Accel = stress ? StressAccel : null,
        CpuModel = cpu,
        Smp = arch == QemuArch.X86_64 ? (smp ?? (stress ? X86_64StressSmp : X86_64Smp)) : smp,
        BootUsb = usb,
        ForeignDisk = foreignDisk,
        Arch = arch,
    };

    /// <summary>
    /// Boots, polls the log, and stops as soon as the verdict is decided or the
    /// time is up. Kills only the QEMU it started: killing by process name
    /// would take down any other emulator on the machine.
    /// </summary>
    public static BootVerdict Run(string image, string serialLog, TimeSpan timeout, bool full, Action<string>? progress = null, string? cpu = null, UsbHost? usb = null, string? foreignDisk = null, QemuArch arch = QemuArch.X86_64, string? smp = null, bool stress = false)
        => Run(image, serialLog, timeout, log => BootVerdict.Classify(log, full, results: arch == QemuArch.X86_64, notJudged: stress ? StressNotJudged : null), progress, cpu, snapshot: true, usb, foreignDisk, arch, smp, stress);

    /// <summary>
    /// The same boot with the verdict decided by <paramref name="classify"/>,
    /// and, with <paramref name="snapshot"/> false, writing to the image: for
    /// a test that needs what one boot left on disk, such as the crash log
    /// (<c>cxk verify-crashlog</c>). Only ever on a copy.
    /// </summary>
    public static BootVerdict Run(string image, string serialLog, TimeSpan timeout, Func<string, BootVerdict> classify,
                                  Action<string>? progress, string? cpu, bool snapshot, UsbHost? usb = null, string? foreignDisk = null, QemuArch arch = QemuArch.X86_64, string? smp = null, bool stress = false)
    {
        if (File.Exists(serialLog)) File.Delete(serialLog);

        var psi = new ProcessStartInfo
        {
            FileName = ExecutableResolver.Resolve(QemuTool.ExecutableFor(arch)),
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (string a in QemuTool.BuildArguments(ConfigFor(image, serialLog, cpu, snapshot, usb, foreignDisk, arch, smp, stress))) psi.ArgumentList.Add(a);

        using var qemu = Process.Start(psi)
            ?? throw new InvalidOperationException($"could not start {QemuTool.ExecutableFor(arch)}");
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
                verdict = classify(log);
                // A pass is read once more a second later: what the prompt
                // is followed by (a refusal, a panic) lands just after it.
                if (verdict.Outcome == BootOutcome.Pass)
                {
                    Thread.Sleep(1000);
                    verdict = classify(ReadShared(serialLog));
                }
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
