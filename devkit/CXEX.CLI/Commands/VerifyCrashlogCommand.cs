// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Disk;
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// The crash log, end to end (roadmap step 4): a deliberate panic's log must
/// be on disk after the panic, and the next boot must recover it and clear
/// the area. Two boots of a copy of the image, since the second needs what the
/// first wrote; the image itself is never touched.
///
/// <para>The panic is asked for through the crash area itself: a request
/// record (kernel/diag/crashrec.h), which the kernel clears before anything
/// else and acts on after its self-tests. Nothing else can arm it, so an
/// ordinary boot never panics on purpose.</para>
/// </summary>
public class VerifyCrashlogCommand : Command<VerifyCrashlogCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE_PATH>")]
        [Description("The disk image. A copy is booted twice; the image is never modified")]
        public string ImagePath { get; set; } = string.Empty;

        [CommandOption("-t|--timeout")]
        [Description("Seconds to wait for each boot's verdict")]
        [DefaultValue(240)]
        public int TimeoutSeconds { get; set; } = 240;

        [CommandOption("--cpu")]
        [Description("QEMU CPU model. Default: QEMU's own")]
        public string? Cpu { get; set; }

        [CommandOption("--usb")]
        [Description("Boot the copy as a USB stick on this host controller: xhci, ehci or ohci. The first boot then leaves a transfer half done before it panics, and the save must reset the stick. Default: IDE")]
        public string? Usb { get; set; }
    }

    private const string Reason = BootVerdict.CrashTestReason;

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.ImagePath))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] disk image not found: '{Markup.Escape(settings.ImagePath)}'");
            return 1;
        }
        UsbHost? usb = null;
        if (settings.Usb is not null && (usb = QemuTool.ParseUsbHost(settings.Usb)) is null)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --usb takes xhci, ehci or ohci, not '{Markup.Escape(settings.Usb)}'");
            return 1;
        }

        string copy = settings.ImagePath + ".crashtest.img";
        string log1 = settings.ImagePath + ".crashtest1.serial.log";
        string log2 = settings.ImagePath + ".crashtest2.serial.log";
        var timeout = TimeSpan.FromSeconds(settings.TimeoutSeconds);
        File.Copy(settings.ImagePath, copy, overwrite: true);

        try
        {
            using (var fs = new FileStream(copy, FileMode.Open, FileAccess.ReadWrite))
            {
                if (CrashLog.FindArea(fs) is null) return Verdict(BootOutcome.Fail, "the image has no CRASH partition", null);
                CrashLog.Arm(fs);
            }

            AnsiConsole.MarkupLine($"[grey]boot 1: armed, expecting a panic (serial -> {Markup.Escape(log1)})[/]");
            var v1 = BootVerifier.Run(copy, log1, timeout, BootVerdict.ClassifyRequestedPanic, null, settings.Cpu, snapshot: false, usb);
            if (v1.Outcome != BootOutcome.Pass) return Verdict(v1.Outcome, "boot 1: " + v1.Reason, log1);

            CrashAreaReading saved;
            using (var fs = new FileStream(copy, FileMode.Open, FileAccess.Read)) saved = CrashLog.Read(fs);
            if (saved.State != CrashAreaState.Record)
                return Verdict(BootOutcome.Fail, "after boot 1 the crash area holds no record: " + saved.Detail, log1);
            // On USB the first boot left a transfer half done before panicking
            // (crashlog_test_interrupt), so the save must have reset the stick.
            var wants = new List<string> { "*** KERNEL PANIC ***", Reason, "Stack trace", "---- system log" };
            if (usb is not null) wants.Add(CrashLog.UsbResetLine);
            foreach (string want in wants)
                if (!saved.Text.Contains(want))
                    return Verdict(BootOutcome.Fail, $"the saved record lacks \"{want}\"", log1);

            AnsiConsole.MarkupLine($"[grey]boot 1: {Markup.Escape(saved.Detail)}[/]");
            AnsiConsole.MarkupLine($"[grey]boot 2: expecting the record recovered (serial -> {Markup.Escape(log2)})[/]");
            var v2 = BootVerifier.Run(copy, log2, timeout, l => BootVerdict.Classify(l, full: false), null, settings.Cpu, snapshot: false, usb);
            if (v2.Outcome != BootOutcome.Pass) return Verdict(v2.Outcome, "boot 2: " + v2.Reason, log2);

            // The console wraps at its width, so a line can arrive in pieces.
            string boot2 = File.ReadAllText(log2).Replace("\r", "").Replace("\n", "");
            if (!boot2.Contains("recovered the previous boot's crash log") || !boot2.Contains(Reason))
                return Verdict(BootOutcome.Fail, "boot 2 did not report recovering the crash log", log2);

            using (var fs = new FileStream(copy, FileMode.Open, FileAccess.Read))
            {
                var after = CrashLog.Read(fs);
                if (after.State != CrashAreaState.Empty)
                    return Verdict(BootOutcome.Fail, "after boot 2 the crash area is not clear: " + after.Detail, log2);
            }

            File.Delete(copy);
            return Verdict(BootOutcome.Pass, $"the panic's log was saved ({saved.Text.Length} bytes), recovered by the next boot, and the area cleared; {v2.Reason}", null);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] could not start {QemuTool.Executable}: {Markup.Escape(ex.Message)}");
            return 1;
        }
    }

    private static int Verdict(BootOutcome o, string reason, string? log)
    {
        string what = o switch { BootOutcome.Pass => "[green]VERDICT: PASS[/]", BootOutcome.Fail => "[red]VERDICT: FAIL[/]", _ => "[yellow]VERDICT: INCONCLUSIVE[/]" };
        AnsiConsole.MarkupLine($"{what} - {Markup.Escape(reason)}");
        if (log is not null) AnsiConsole.MarkupLine($"[grey]  log: {Markup.Escape(log)}[/]");
        return (int)o;
    }
}

/// <summary>
/// Prints the crash log a panic left on a disk, without booting it
/// (docs/devkit/CX_CXK_PLAN.md section 3.7), from an image file or straight
/// from the rig's USB stick (<see cref="RigDisk"/>).
/// </summary>
public class RigCrashlogCommand : Command<RigCrashlogCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<DISK>")]
        [Description(RigDisk.Help)]
        public string DiskPath { get; set; } = string.Empty;
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        CrashAreaReading r;
        using (var disk = RigDisk.Open(settings.DiskPath))
        {
            if (disk is null) return 1;
            r = CrashLog.Read(disk);
        }

        switch (r.State)
        {
            case CrashAreaState.Record:
            case CrashAreaState.FaultRecord:
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(r.Detail)}[/]");
                Console.Out.Write(r.Text.Replace("\r", ""));
                if (!r.Text.EndsWith('\n')) Console.Out.WriteLine();
                return 0;
            case CrashAreaState.Invalid:
                AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(r.Detail)}");
                return 1;
            default:
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(r.Detail)}[/]");
                return 2;
        }
    }
}

/// <summary>
/// <c>cxk rig bootlog &lt;disk&gt; [-o file]</c>: the system log the last boot saved
/// to the disk's CRASH partition, printed or written to a file. The kernel saves
/// it at the end of the boot and again every few seconds while the log grows, so
/// it covers the boot, the services, and whatever came after.
/// </summary>
public class RigBootlogCommand : Command<RigBootlogCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<DISK>")]
        [Description(RigDisk.Help)]
        public string DiskPath { get; set; } = string.Empty;

        [CommandOption("-o|--out <FILE>")]
        [Description("Write the log to this file instead of printing it")]
        public string? OutPath { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        CrashAreaReading r;
        using (var disk = RigDisk.Open(settings.DiskPath))
        {
            if (disk is null) return 1;
            r = CrashLog.ReadBootLog(disk);
        }

        switch (r.State)
        {
            case CrashAreaState.Record:
                if (settings.OutPath is string o)
                {
                    if (RawDisk.DevicePath(o) is not null)
                    {
                        AnsiConsole.MarkupLine("[red]error:[/] the output is a disk; rig bootlog only writes files");
                        return 1;
                    }
                    File.WriteAllText(o, r.Text.Replace("\r", ""));
                    AnsiConsole.MarkupLine($"[green]wrote[/] {Markup.Escape(r.Detail)} to {Markup.Escape(o)}");
                    return 0;
                }
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(r.Detail)}[/]");
                Console.Out.Write(r.Text.Replace("\r", ""));
                if (!r.Text.EndsWith('\n')) Console.Out.WriteLine();
                return 0;
            case CrashAreaState.Invalid:
                AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(r.Detail)}");
                return 1;
            default:
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(r.Detail)}[/]");
                return 2;
        }
    }
}
