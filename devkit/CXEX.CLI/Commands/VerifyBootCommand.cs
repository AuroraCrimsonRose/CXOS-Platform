// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// Boots an image headless and reports the verdict from the serial console:
/// PASS, FAIL or INCONCLUSIVE, as exit codes 0, 1 and 2. The gate a kernel
/// change has to pass, and the step that lets the CI image prove Linux can
/// build CXOS *and* boot it. Replaces serialboot.ps1 and bootverify.py, which
/// lived outside the repository (HARDENING_PLAN D2).
/// </summary>
public class VerifyBootCommand : Command<VerifyBootCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE_PATH>")]
        [Description("The disk image to boot. Booted with -snapshot: the image is never modified")]
        public string ImagePath { get; set; } = string.Empty;

        [CommandOption("--full")]
        [Description("Also wait for the executive to start, not just the kernel self-tests")]
        public bool Full { get; set; }

        [CommandOption("-t|--timeout")]
        [Description("Seconds to wait for a verdict before calling it inconclusive")]
        [DefaultValue(120)]
        public int TimeoutSeconds { get; set; } = 120;

        [CommandOption("--cpu")]
        [Description("QEMU CPU model, e.g. pentium (minimal) or max (every feature). Default: QEMU's own")]
        public string? Cpu { get; set; }

        [CommandOption("--log")]
        [Description("Where to write the serial log. Default: beside the image, <image>.serial.log")]
        public string? LogPath { get; set; }

        [CommandOption("--usb")]
        [Description("Boot the image as a USB stick on this host controller: xhci, ehci or ohci. Default: IDE")]
        public string? Usb { get; set; }

        [CommandOption("--foreign-disk")]
        [Description("Put a copy of the image under another disk id ahead of it, as a machine's own drive with CXOS on it already, and fail if the boot changes a byte of it")]
        public bool ForeignDisk { get; set; }

        [CommandOption("--arch")]
        [Description("The kernel the image carries: x86_32 (default) or x86_64, which boots it on qemu-system-x86_64")]
        public string? Arch { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.ImagePath))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] disk image not found: '{Markup.Escape(settings.ImagePath)}'");
            return 1;
        }

        if (settings.Cpu is { Length: > 0 } c && (c[0] == '-' || !c.All(ch => char.IsAsciiLetterOrDigit(ch) || "_-.,+=".Contains(ch))))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] not a CPU model: '{Markup.Escape(c)}'");
            return 1;
        }

        UsbHost? usb = null;
        if (settings.Usb is not null && (usb = QemuTool.ParseUsbHost(settings.Usb)) is null)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] --usb takes xhci, ehci or ohci, not '{Markup.Escape(settings.Usb)}'");
            return 1;
        }

        QemuArch arch = QemuArch.X86_32;
        if (settings.Arch is not null)
        {
            if (QemuTool.ParseArch(settings.Arch) is not QemuArch a)
            {
                AnsiConsole.MarkupLine($"[red]error:[/] --arch takes x86_32 or x86_64, not '{Markup.Escape(settings.Arch)}'");
                return 1;
            }
            arch = a;
        }

        string log = settings.LogPath is { Length: > 0 } l ? l : settings.ImagePath + ".serial.log";
        string? foreign = settings.ForeignDisk ? settings.ImagePath + ".foreign.img" : null;
        byte[]? foreignHash = foreign is null ? null : CXEX.Tools.ForeignDisk.Create(foreign, File.ReadAllBytes(settings.ImagePath));
        long foreignSize = foreign is null ? 0 : new FileInfo(foreign).Length;
        AnsiConsole.MarkupLine($"[grey]booting[/] {Markup.Escape(settings.ImagePath)} [grey](snapshot, serial -> {Markup.Escape(log)})[/]");
        AnsiConsole.MarkupLine($"[grey]{Markup.Escape(QemuTool.CommandLine(BootVerifier.ConfigFor(settings.ImagePath, log, settings.Cpu, usb: usb, foreignDisk: foreign, arch: arch)))}[/]");

        BootVerdict v;
        try
        {
            v = BootVerifier.Run(settings.ImagePath, log, TimeSpan.FromSeconds(settings.TimeoutSeconds), settings.Full,
                                 line => AnsiConsole.MarkupLine($"[grey]{Markup.Escape(line)}[/]"), settings.Cpu, usb, foreign, arch);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] could not start {QemuTool.ExecutableFor(arch)}: {Markup.Escape(ex.Message)}");
            if (foreign is not null) DeleteQuietly(foreign);   // nothing ran, so nothing to keep
            return 1;
        }

        // Decided after the boot whatever its verdict: a write to a disk that
        // is not CXOS's fails the boot even if every self-test passed.
        if (foreign is not null && foreignHash is not null)
        {
            bool? unchanged = CXEX.Tools.ForeignDisk.Unchanged(foreign, foreignHash);
            if (unchanged is null)
                v = v with { Outcome = BootOutcome.Inconclusive, Reason = $"the foreign disk could not be read back (kept at {foreign})" };
            else if (unchanged is false)
                v = v with { Outcome = BootOutcome.Fail, Reason = $"the boot wrote to the foreign disk (kept at {foreign})" };
            else
            {
                DeleteQuietly(foreign);
                bool seen = CXEX.Tools.ForeignDisk.Seen(File.Exists(log) ? File.ReadAllText(log) : "", foreignSize);
                if (!seen && v.Outcome == BootOutcome.Pass)
                    v = v with { Outcome = BootOutcome.Inconclusive, Reason = "the kernel never named the foreign disk as left read-only: it was never seen" };
                else if (v.Outcome == BootOutcome.Pass)
                    v = v with { Reason = v.Reason + "; the foreign disk was seen, refused and is unchanged" };
            }
        }

        switch (v.Outcome)
        {
            case BootOutcome.Pass:
                AnsiConsole.MarkupLine($"[green]VERDICT: PASS[/] - {Markup.Escape(v.Reason)}");
                return 0;
            case BootOutcome.Fail:
                AnsiConsole.MarkupLine($"[red]VERDICT: FAIL[/] - {Markup.Escape(v.Reason)}");
                AnsiConsole.MarkupLine($"[grey]  full log: {Markup.Escape(log)}[/]");
                return 1;
            default:
                AnsiConsole.MarkupLine($"[yellow]VERDICT: INCONCLUSIVE[/] - {Markup.Escape(v.Reason)}");
                AnsiConsole.MarkupLine($"[grey]  full log: {Markup.Escape(log)}[/]");
                return 2;
        }
    }

    // A 32 MB scratch file left behind is untidy, not a reason to lose the verdict.
    private static void DeleteQuietly(string path)
    {
        try { File.Delete(path); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
