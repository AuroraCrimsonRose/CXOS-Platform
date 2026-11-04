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
/// The random generator's seed file, end to end (roadmap 1.7c,
/// docs/security/CX_RANDOM.md §5): a boot leaves a seed in the crash
/// partition, and the next boot credits it, having first rewritten it with
/// another. Two boots of a copy of the image, since the second needs what the
/// first wrote; the image itself is never touched. A fresh image carries no
/// seed, so the first boot must credit none.
/// </summary>
public class VerifySeedCommand : Command<VerifySeedCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE_PATH>")]
        [Description("The disk image. A copy is booted twice; the image is never modified")]
        public string ImagePath { get; set; } = string.Empty;

        [CommandOption("-t|--timeout")]
        [Description("Seconds to wait for each boot's verdict")]
        [DefaultValue(600)]
        public int TimeoutSeconds { get; set; } = 600;

        [CommandOption("--cpu")]
        [Description("QEMU CPU model. Default: QEMU's own")]
        public string? Cpu { get; set; }
    }

    /// <summary>What random_seed_file_load logs when it credits a seed (kernel/drivers/char/random.c).</summary>
    public const string CreditedLine = "seed file read back rewritten, credited bits 256";

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.ImagePath))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] disk image not found: '{Markup.Escape(settings.ImagePath)}'");
            return 1;
        }
        string copy = settings.ImagePath + ".seedtest.img";
        string log1 = settings.ImagePath + ".seedtest1.serial.log";
        string log2 = settings.ImagePath + ".seedtest2.serial.log";
        var timeout = TimeSpan.FromSeconds(settings.TimeoutSeconds);
        File.Copy(settings.ImagePath, copy, overwrite: true);

        try
        {
            using (var fs = new FileStream(copy, FileMode.Open, FileAccess.Read))
            {
                if (CrashLog.FindArea(fs) is null) return Verdict(BootOutcome.Fail, "the image has no CRASH partition", null);
                if (CrashLog.ReadSeed(fs) is not null) return Verdict(BootOutcome.Fail, "the image already carries a seed: every disk written from it would share it", null);
            }

            AnsiConsole.MarkupLine($"[grey]boot 1: no seed yet (serial -> {Markup.Escape(log1)})[/]");
            var v1 = BootVerifier.Run(copy, log1, timeout, l => BootVerdict.Classify(l, full: false), null, settings.Cpu, snapshot: false);
            if (v1.Outcome != BootOutcome.Pass) return Verdict(v1.Outcome, "boot 1: " + v1.Reason, log1);
            if (Joined(log1).Contains(CreditedLine)) return Verdict(BootOutcome.Fail, "boot 1 credited a seed the image never had", log1);

            byte[]? seed1;
            using (var fs = new FileStream(copy, FileMode.Open, FileAccess.Read)) seed1 = CrashLog.ReadSeed(fs);
            if (seed1 is null) return Verdict(BootOutcome.Fail, "after boot 1 the crash area holds no seed for the next boot", log1);

            AnsiConsole.MarkupLine($"[grey]boot 2: expecting boot 1's seed credited (serial -> {Markup.Escape(log2)})[/]");
            var v2 = BootVerifier.Run(copy, log2, timeout, l => BootVerdict.Classify(l, full: false), null, settings.Cpu, snapshot: false);
            if (v2.Outcome != BootOutcome.Pass) return Verdict(v2.Outcome, "boot 2: " + v2.Reason, log2);
            if (!Joined(log2).Contains(CreditedLine)) return Verdict(BootOutcome.Fail, "boot 2 did not credit the seed boot 1 left", log2);

            byte[]? seed2;
            using (var fs = new FileStream(copy, FileMode.Open, FileAccess.Read)) seed2 = CrashLog.ReadSeed(fs);
            if (seed2 is null) return Verdict(BootOutcome.Fail, "after boot 2 the crash area holds no seed", log2);
            if (seed2.AsSpan().SequenceEqual(seed1)) return Verdict(BootOutcome.Fail, "boot 2 left the seed it was given: the next boot would credit it again", log2);

            File.Delete(copy);
            return Verdict(BootOutcome.Pass, $"boot 1 left a seed, boot 2 credited it and left another; {v2.Reason}", null);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] could not start {QemuTool.Executable}: {Markup.Escape(ex.Message)}");
            return 1;
        }
    }

    /// <summary>The serial log as one line: the console wraps at its width, so a
    /// line can arrive in pieces. Only the boot's own load logs its seed lines;
    /// the self-test's, against a test generator, log nothing.</summary>
    private static string Joined(string log) => File.ReadAllText(log).Replace("\r", "").Replace("\n", "");

    private static int Verdict(BootOutcome o, string reason, string? log)
    {
        string what = o switch { BootOutcome.Pass => "[green]VERDICT: PASS[/]", BootOutcome.Fail => "[red]VERDICT: FAIL[/]", _ => "[yellow]VERDICT: INCONCLUSIVE[/]" };
        AnsiConsole.MarkupLine($"{what} - {Markup.Escape(reason)}");
        if (log is not null) AnsiConsole.MarkupLine($"[grey]  log: {Markup.Escape(log)}[/]");
        return (int)o;
    }
}
