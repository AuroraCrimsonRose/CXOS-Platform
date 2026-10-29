// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.IO.Pipes;

namespace CXEX.Tools;

/// <summary>
/// Boots a CXOS disk image in a Hyper-V Generation 1 VM and judges its serial
/// console as <see cref="BootVerifier"/> does: a second firmware after QEMU's
/// SeaBIOS, an AMI BIOS on IDE, before the rig's own (step 1.3a'). The image is
/// converted to a VHDX copy with qemu-img, so it is never modified; COM1 goes to
/// a named pipe this class reads into the log. The VM and the copy are removed
/// afterwards whatever the verdict.
///
/// <para>Needs Windows with Hyper-V, and the user in "Hyper-V Administrators".
/// The real rig stays the final word.</para>
/// </summary>
public static class HyperVBoot
{
    public sealed record Options(int MemoryMb = 2048, int Processors = 1, bool Keep = false);

    /// <summary>The name of the VM and its pipe: only letters, digits and dashes, so it is safe in a script.</summary>
    public static string NewVmName() => "cxk-verify-" + Guid.NewGuid().ToString("N")[..12];

    /// <summary>
    /// The PowerShell that creates and starts the VM. Every value it takes is
    /// either a number or checked here, and paths are single-quoted with their
    /// quotes doubled, so nothing in a path runs as script.
    /// </summary>
    public static string CreateScript(string vmName, string vmDir, string vhdx, string pipe, Options o)
    {
        if (!IsSafeName(vmName)) throw new ArgumentException($"not a safe VM name: '{vmName}'", nameof(vmName));
        if (!IsSafeName(pipe)) throw new ArgumentException($"not a safe pipe name: '{pipe}'", nameof(pipe));
        if (o.MemoryMb < 256 || o.MemoryMb > 65536) throw new ArgumentOutOfRangeException(nameof(o), "memory: 256 to 65536 MB");
        if (o.Processors < 1 || o.Processors > 64) throw new ArgumentOutOfRangeException(nameof(o), "processors: 1 to 64");

        return string.Join("; ",
            "$ErrorActionPreference = 'Stop'",
            $"$vm = New-VM -Name '{vmName}' -Generation 1 -MemoryStartupBytes {o.MemoryMb}MB -NoVHD -Path {Quote(vmDir)}",
            $"Set-VMMemory -VM $vm -DynamicMemoryEnabled $false",
            $"Set-VMProcessor -VM $vm -Count {o.Processors}",
            "Set-VM -VM $vm -AutomaticCheckpointsEnabled $false -CheckpointType Disabled -AutomaticStartAction Nothing -AutomaticStopAction TurnOff",
            $"Add-VMHardDiskDrive -VM $vm -ControllerType IDE -ControllerNumber 0 -ControllerLocation 0 -Path {Quote(vhdx)}",
            $@"Set-VMComPort -VM $vm -Number 1 -Path '\\.\pipe\{pipe}'",
            "Set-VMBios -VM $vm -StartupOrder @('IDE', 'CD', 'LegacyNetworkAdapter', 'Floppy')",
            "Start-VM -VM $vm");
    }

    /// <summary>Turns the VM off and deletes it; Hyper-V leaves the disk file, which the caller deletes.</summary>
    public static string RemoveScript(string vmName)
    {
        if (!IsSafeName(vmName)) throw new ArgumentException($"not a safe VM name: '{vmName}'", nameof(vmName));
        return string.Join("; ",
            $"$vm = Get-VM -Name '{vmName}' -ErrorAction SilentlyContinue",
            "if ($vm) { Stop-VM -VM $vm -TurnOff -Force -ErrorAction SilentlyContinue; Remove-VM -VM $vm -Force }");
    }

    private static bool IsSafeName(string s) =>
        s.Length is > 0 and <= 64 && s.All(c => char.IsAsciiLetterOrDigit(c) || c == '-');

    private static string Quote(string path) => "'" + path.Replace("'", "''") + "'";

    /// <summary>
    /// Boots <paramref name="image"/> and returns the verdict. <paramref name="progress"/>
    /// gets one line per step; the serial console goes to <paramref name="serialLog"/>.
    /// </summary>
    public static BootVerdict Run(string image, string serialLog, TimeSpan timeout, bool full, Options options,
                                  Action<string>? progress = null)
    {
        if (!OperatingSystem.IsWindows())
            return new BootVerdict(BootOutcome.Inconclusive, "Hyper-V needs Windows", null);

        string vmName = NewVmName();
        string work = Path.Combine(Path.GetTempPath(), vmName);
        string vhdx = Path.Combine(work, "cxk.vhdx");
        Directory.CreateDirectory(work);
        bool created = false;
        try
        {
            progress?.Invoke($"converting to VHDX: {vhdx}");
            var conv = ToolProcess.Run("qemu-img", ["convert", "-f", "raw", "-O", "vhdx", "-o", "subformat=dynamic", image, vhdx],
                                       new ToolRunOptions { Timeout = TimeSpan.FromMinutes(5) });
            if (!conv.Ok)
                return new BootVerdict(BootOutcome.Inconclusive, "qemu-img could not convert the image: " + conv.Describe(), null);
            // qemu-img writes a sparse file on NTFS, and Hyper-V will not attach
            // one ("virtual hard disk files must ... not be sparse").
            var dense = ToolProcess.Run("fsutil", ["sparse", "setflag", vhdx, "0"]);
            if (!dense.Ok)
                return new BootVerdict(BootOutcome.Inconclusive, "could not clear the VHDX's sparse flag: " + dense.Describe(), null);

            progress?.Invoke($"creating and starting VM {vmName} (Gen 1, {options.MemoryMb} MB, COM1 -> \\\\.\\pipe\\{vmName})");
            created = true;   // from here on, try to remove it even if creation failed half-way
            var start = PowerShell(CreateScript(vmName, work, vhdx, vmName, options));
            if (!start.Ok)
                return new BootVerdict(BootOutcome.Inconclusive, "Hyper-V refused the VM: " + FirstLines(start.StandardError, start.Describe()), null);

            return Watch(vmName, serialLog, timeout, full);
        }
        finally
        {
            if (created && !options.Keep)
            {
                var rm = PowerShell(RemoveScript(vmName));
                if (!rm.Ok) progress?.Invoke($"could not remove VM {vmName}: {FirstLines(rm.StandardError, rm.Describe())}");
            }
            if (!options.Keep)
            {
                try { Directory.Delete(work, recursive: true); }
                catch (IOException e) { progress?.Invoke($"left {work}: {e.Message}"); }
                catch (UnauthorizedAccessException e) { progress?.Invoke($"left {work}: {e.Message}"); }
            }
            else progress?.Invoke($"kept VM {vmName} and {work}");
        }
    }

    /// <summary>Copies the pipe into the log and classifies it until a verdict or the timeout.</summary>
    private static BootVerdict Watch(string pipe, string serialLog, TimeSpan timeout, bool full)
    {
        var clock = Stopwatch.StartNew();
        using var log = new FileStream(serialLog, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
        using var client = new NamedPipeClientStream(".", pipe, PipeDirection.In, PipeOptions.Asynchronous);
        try { client.Connect((int)Math.Min(timeout.TotalMilliseconds, 30_000)); }
        catch (TimeoutException) { return new BootVerdict(BootOutcome.Inconclusive, "the VM's COM1 pipe never opened", null); }

        var text = new System.Text.StringBuilder();
        var buf = new byte[4096];
        BootVerdict verdict = new(BootOutcome.Inconclusive, "the VM produced no serial output", null);
        while (clock.Elapsed < timeout)
        {
            var read = client.ReadAsync(buf, 0, buf.Length);
            if (!read.Wait(500))
            {
                // nothing new; the VM may have stopped writing for good
                if (verdict.Outcome != BootOutcome.Inconclusive) break;
                continue;
            }
            int n = read.Result;
            if (n == 0) { verdict = verdict with { Reason = "the VM closed COM1 before a verdict: " + verdict.Reason }; break; }
            log.Write(buf, 0, n);
            log.Flush();
            text.Append(System.Text.Encoding.Latin1.GetString(buf, 0, n));
            verdict = BootVerdict.Classify(text.ToString(), full);
            if (verdict.Outcome != BootOutcome.Inconclusive) return verdict;
        }
        if (verdict.Outcome == BootOutcome.Inconclusive && clock.Elapsed >= timeout)
            verdict = verdict with { Reason = $"no verdict within {timeout.TotalSeconds:0} s: {verdict.Reason}" };
        return verdict;
    }

    private static ToolResult PowerShell(string script) =>
        ToolProcess.Run("powershell.exe", ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script],
                        new ToolRunOptions { Timeout = TimeSpan.FromMinutes(2) });

    private static string FirstLines(string s, string fallback)
    {
        var lines = s.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Take(3);
        string joined = string.Join(" / ", lines);
        return joined.Length > 0 ? joined : fallback;
    }
}
