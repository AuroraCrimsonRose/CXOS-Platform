// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Disk;
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Globalization;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// Where a hardware report comes from (roadmap 1.8b): a serial log or a saved
/// report (the <c>@hw</c> lines in it), or a disk, an image file or the rig's
/// stick, whose crash partition holds the last boot's report (XCRL kind 6).
/// </summary>
internal static class HwSource
{
    public const string Help = "A serial log or a saved report (its @hw lines), or a disk whose CRASH partition holds the last boot's report: an image file, disk5, \\\\.\\PhysicalDrive5, /dev/sdb. Read-only";

    /// <summary>The report, or null with the reason already printed.</summary>
    public static HwReport? Load(string arg, bool profile = false)
    {
        string? text = null;
        if (profile)
        {
            // the self-tests' profile goes to serial only (1.9f)
            if (RawDisk.DevicePath(arg) is not null || !File.Exists(arg)) return Fail(arg, "a profile is read from a serial log, and this is not a file");
            text = HwReport.ProfileFromSerial(File.ReadAllText(arg));
            if (text is null) return Fail(arg, "it holds no profile (a test-hook boot prints one after the self-tests)");
            if (HwReport.Check(text) is string bad) return Fail(arg, "the profile is damaged: " + bad);
            return HwReport.Parse(text);
        }
        if (RawDisk.DevicePath(arg) is null && File.Exists(arg))
        {
            bool disk;
            using (var fs = new FileStream(arg, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                disk = CrashLog.FindArea(fs) is not null;
            if (!disk)
            {
                text = HwReport.TextFromSerial(File.ReadAllText(arg));
                if (text is null) return Fail(arg, "it holds no \"@hw begin\" line, and it is not a disk with a CRASH partition");
            }
        }
        if (text is null)
        {
            CrashAreaReading r;
            using (var disk = RigDisk.Open(arg))
            {
                if (disk is null) return null;
                r = CrashLog.ReadHwReport(disk);
            }
            if (r.State != CrashAreaState.Record) return Fail(arg, r.Detail);
            text = r.Text;
        }
        if (HwReport.Check(text) is string wrong) return Fail(arg, "the report is damaged: " + wrong);
        return HwReport.Parse(text);
    }

    private static HwReport? Fail(string arg, string why)
    {
        AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(arg)}: {Markup.Escape(why)}");
        return null;
    }
}

/// <summary><c>cxk hw show &lt;source&gt;</c>: a report, each line's fields by name, and what it says is wrong.</summary>
public class HwShowCommand : Command<HwShowCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCE>")]
        [Description(HwSource.Help)]
        public string Source { get; set; } = string.Empty;

        [CommandOption("-s|--section <NAME>")]
        [Description("Only this section's lines: boot, cpu, cpuid, hypervisor, apic, mem, acpi, madt, pci, clock")]
        public string? Section { get; set; }

        [CommandOption("--raw")]
        [Description("Print the report's text as the kernel wrote it")]
        public bool Raw { get; set; }

        [CommandOption("--profile")]
        [Description("The self-tests' profile in a serial log instead (1.9f): the counted locks' contention")]
        public bool Profile { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        var r = HwSource.Load(settings.Source, settings.Profile);
        if (r is null) return 1;
        if (settings.Raw)
        {
            Console.Out.Write(r.Text);
            return 0;
        }

        var facts = HwDiff.Facts(r).Where(f => settings.Section is null || f.Section == settings.Section || f.Section == settings.Section + "sum").ToList();
        int width = facts.Count == 0 ? 0 : facts.Max(f => f.Key.Length);
        AnsiConsole.MarkupLine($"[grey]hardware report, {r.Lines.Count} lines{(r.Truncated ? ", cut" : "")}[/]");
        foreach (var f in facts)
            Console.Out.WriteLine(f.Key.PadRight(width) + "  " + string.Join("  ", f.Fields.Select(x => $"{x.Name}={x.Value}")));
        foreach (string n in HwDiff.Notes(r))
            AnsiConsole.MarkupLine($"[yellow]note:[/] {Markup.Escape(n)}");
        return 0;
    }
}

/// <summary>
/// <c>cxk hw diff &lt;a&gt; &lt;b&gt;</c>: two reports compared fact by fact.
/// Exit 0 when every difference is expected, 1 on a finding, 2 when a report
/// could not be read.
/// </summary>
public class HwDiffCommand : Command<HwDiffCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<A>")]
        [Description(HwSource.Help)]
        public string A { get; set; } = string.Empty;

        [CommandArgument(1, "<B>")]
        [Description("The report to compare with A, from the same kinds of source")]
        public string B { get; set; } = string.Empty;

        [CommandOption("--all")]
        [Description("Also list the expected differences (firmware-chosen addresses, hypervisor leaves, rates within the tolerance)")]
        public bool All { get; set; }

        [CommandOption("--rate-tolerance <PERCENT>")]
        [Description("How far two measured rates may be apart and still be expected")]
        [DefaultValue(1.0)]
        public double RateTolerance { get; set; } = 1.0;

        [CommandOption("--profile")]
        [Description("Compare the self-tests' profiles in two serial logs instead (1.9f): every line set beside the other, all of them measures")]
        public bool Profile { get; set; }
    }

    protected override ValidationResult Validate(CommandContext context, Settings settings) =>
        settings.RateTolerance is >= 0 and <= 100 ? ValidationResult.Success() : ValidationResult.Error("--rate-tolerance is a percentage, 0 to 100");

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        var a = HwSource.Load(settings.A, settings.Profile);
        var b = a is null ? null : HwSource.Load(settings.B, settings.Profile);
        if (settings.Profile) settings.All = true;   // a profile is nothing but measures
        if (a is null || b is null) return 2;

        var diffs = HwDiff.Compare(a, b, settings.RateTolerance / 100);
        int findings = diffs.Count(d => !d.Expected);
        foreach (var d in diffs.Where(d => settings.All || !d.Expected))
        {
            string where = d.Field is null ? d.Key : $"{d.Key} {d.Field}";
            string what = d.Field is null ? d.A is null ? $"B: {d.B}" : $"A: {d.A}" : $"{d.A} -> {d.B}";
            string tag = d.Expected ? "[grey]expected[/]" : "[red]finding [/]";
            AnsiConsole.MarkupLine($"{tag} {Markup.Escape(where)}: {Markup.Escape(what)} [grey]({Markup.Escape(d.Why)})[/]");
        }
        foreach (var (name, r) in new[] { ("A", a), ("B", b) })
            foreach (string n in HwDiff.Notes(r, settings.RateTolerance / 100))
                AnsiConsole.MarkupLine($"[yellow]note {name}:[/] {Markup.Escape(n)}");
        string summary = string.Create(CultureInfo.InvariantCulture,
            $"{findings} finding(s), {diffs.Count - findings} expected difference(s){(settings.All || diffs.Count == findings ? "" : " (--all lists them)")}");
        AnsiConsole.MarkupLine(findings == 0 ? $"[green]{Markup.Escape(summary)}[/]" : Markup.Escape(summary));
        return findings == 0 ? 0 : 1;
    }
}

/// <summary>
/// <c>cxk rig hwreport &lt;disk&gt; [-o file]</c>: the hardware report the
/// last boot saved to the disk's CRASH partition (sector 384, kind 6), checked
/// and printed or written to a file that <c>cxk hw show</c> and <c>hw diff</c>
/// read. How the rig's report, which may have no serial port to go to, gets off
/// its stick.
/// </summary>
public class RigHwreportCommand : Command<RigHwreportCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<DISK>")]
        [Description(RigDisk.Help)]
        public string DiskPath { get; set; } = string.Empty;

        [CommandOption("-o|--out <FILE>")]
        [Description("Write the report to this file instead of printing it")]
        public string? OutPath { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        CrashAreaReading r;
        using (var disk = RigDisk.Open(settings.DiskPath))
        {
            if (disk is null) return 1;
            r = CrashLog.ReadHwReport(disk);
        }

        switch (r.State)
        {
            case CrashAreaState.Record:
                if (HwReport.Check(r.Text) is string wrong)
                {
                    AnsiConsole.MarkupLine($"[red]error:[/] the record is whole but the report in it is damaged: {Markup.Escape(wrong)}");
                    return 1;
                }
                if (settings.OutPath is string o)
                {
                    if (RawDisk.DevicePath(o) is not null)
                    {
                        AnsiConsole.MarkupLine("[red]error:[/] the output is a disk; rig hwreport only writes files");
                        return 1;
                    }
                    File.WriteAllText(o, r.Text);
                    AnsiConsole.MarkupLine($"[green]wrote[/] {Markup.Escape(r.Detail)} to {Markup.Escape(o)}");
                    return 0;
                }
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(r.Detail)}[/]");
                Console.Out.Write(r.Text);
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
