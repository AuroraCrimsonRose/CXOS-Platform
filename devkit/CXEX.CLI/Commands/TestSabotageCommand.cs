// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Text.Json;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// Runs a sabotage file (roadmap 1.5d3, safety/CX_SAFETY.md §7): for each
/// sabotage, or each group of them, apply its edits, build, boot with
/// <c>verify-boot --full --json</c>, restore, and judge by which self-tests
/// went red. It owns what every step's hand-written matrix script got wrong
/// at least once: one backup per file per run, restore in every path (a run
/// that dies is restored by the next), an edit's text counted (exactly one
/// match), the tree compared before and after each run, and a failed or
/// stale build reported as itself, never as red.
/// </summary>
public class TestSabotageCommand : Command<TestSabotageCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "[FILE]")]
        [Description("The sabotage file, e.g. tests/sabotage/1.5d2.sab (format: docs/devkit/CX_CXK_PLAN.md §3.4)")]
        public string? File { get; set; }

        [CommandOption("--all")]
        [Description("Every file in tests/sabotage, each sabotage named <file>/<name>: re-proves every recorded check")]
        public bool All { get; set; }

        [CommandOption("--only")]
        [Description("Run only these sabotages, comma-separated (a group runs whole if any of it is named)")]
        public string? Only { get; set; }

        [CommandOption("--dry-run")]
        [Description("Apply and restore each run's edits, checking each applies and the tree comes back; no build, no boot")]
        public bool DryRun { get; set; }

        [CommandOption("--no-baseline")]
        [Description("Skip the clean build and boot that must pass first (a red baseline makes every verdict meaningless)")]
        public bool NoBaseline { get; set; }

        [CommandOption("--key")]
        [Description("The signing key os build uses. Default: test")]
        public string? Key { get; set; }

        [CommandOption("--dev")]
        [Description("Build with os build --dev instead of signing")]
        public bool Dev { get; set; }

        [CommandOption("-t|--timeout")]
        [Description("Seconds each boot gets (verify-boot -t)")]
        [DefaultValue(600)]
        public int TimeoutSeconds { get; set; } = 600;

        [CommandOption("--root")]
        [Description("The repository root. Default: found from the working directory")]
        public string? Root { get; set; }
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken cancellationToken)
    {
        string? root = OsBuildCommand.ResolveRoot(s.Root);
        if (root is null) { AnsiConsole.MarkupLine("[red]error:[/] could not find the repository root (--root)"); return 2; }
        if (s.All == s.File is { Length: > 0 })
        {
            AnsiConsole.MarkupLine("[red]error:[/] name one sabotage file, or --all");
            return 2;
        }
        string[] files = s.All
            ? Directory.Exists(Path.Combine(root, "tests", "sabotage"))
                ? Directory.GetFiles(Path.Combine(root, "tests", "sabotage"), "*.sab").Order(StringComparer.Ordinal).ToArray()
                : []
            : [s.File!];
        if (files.Length == 0) { AnsiConsole.MarkupLine("[red]error:[/] tests/sabotage has no .sab files"); return 2; }
        if (files.FirstOrDefault(f => !System.IO.File.Exists(f)) is string missing)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] no such file: {Markup.Escape(missing)}");
            return 2;
        }
        string summaryName = s.All ? "all" : Path.GetFileNameWithoutExtension(s.File!);

        IReadOnlyList<IReadOnlyList<Sabotage>> runs;
        try
        {
            var all = new List<Sabotage>();
            foreach (string f in files)
            {
                var parsed = SabotageFile.Parse(System.IO.File.ReadAllText(f), f);
                string stem = Path.GetFileNameWithoutExtension(f);
                // with --all, names and groups are a file's own: two files may both say [owner] or group a
                all.AddRange(s.All ? parsed.Select(x => x with { Name = stem + "/" + x.Name, Group = x.Group is null ? null : stem + "/" + x.Group })
                                   : parsed);
            }
            runs = SabotageFile.Runs(all);
            if (s.Only is { Length: > 0 })
            {
                var want = s.Only.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).ToHashSet();
                var unknown = want.Where(w => all.All(x => x.Name != w)).ToList();
                if (unknown.Count > 0) { AnsiConsole.MarkupLine($"[red]error:[/] no sabotage named {Markup.Escape(string.Join(", ", unknown))}"); return 2; }
                runs = runs.Where(r => r.Any(x => want.Contains(x.Name))).ToList();
            }
        }
        catch (FormatException e)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(e.Message)}");
            return 2;
        }

        string state = Path.Combine(root, "build", "sabotage");
        string outDir = Path.Combine(state, summaryName);
        var guard = new TreeGuard(root, Path.Combine(state, "backup"));
        if (guard.Pending)
        {
            var left = guard.Restore();
            AnsiConsole.MarkupLine("[yellow]an earlier run left files sabotaged: restored them first[/]");
            if (left.Count > 0) return Stop("could not restore what an earlier run left", left);
        }
        string? fp0 = guard.Fingerprint();
        if (fp0 is null) return Stop("git could not report the tree's state", []);

        // Ctrl-C kills the build or QEMU child with us; the files go back first.
        ConsoleCancelEventHandler onCancel = (_, _) => guard.Restore();
        Console.CancelKeyPress += onCancel;
        try
        {
            string[] build = s.Dev ? ["--dev"] : ["--key", s.Key ?? "test"];
            Directory.CreateDirectory(outDir);

            if (!s.DryRun && !s.NoBaseline)
            {
                AnsiConsole.MarkupLine("[grey]baseline: the tree as it is, built and booted[/]");
                string dir = Path.Combine(outDir, "baseline");
                Directory.CreateDirectory(dir);
                string? bp = TestSupport.Build(root, build, Path.Combine(dir, "build.log"), kernelChanged: false);
                if (bp is not null) return Stop("baseline: " + bp, []);
                var (boot, problem) = TestSupport.Boot(root, [], dir, s.TimeoutSeconds);
                if (boot is null) return Stop("baseline: " + problem, []);
                if (boot.Outcome != "pass") return Stop($"the baseline is not green ({boot.Outcome}: {boot.Reason}); nothing can be judged against it", []);
                if (guard.Fingerprint() != fp0) return Stop("the build itself changed the tree; a restore check cannot work", []);
            }

            var outcomes = new List<SabotageOutcome>();
            foreach (var run in runs)
            {
                string label = string.Join("+", run.Select(x => x.Name));
                var edits = run.SelectMany(x => x.Edits).ToList();
                string? problem = guard.Apply(edits);
                List<SabotageOutcome> got;
                if (problem is not null)
                    got = run.Select(x => new SabotageOutcome(x.Name, SabotageVerdict.NotApplied, problem)).ToList();
                else if (s.DryRun)
                    got = run.Select(x => new SabotageOutcome(x.Name, SabotageVerdict.Caught,
                        $"applied {x.Edits.Count} edit(s) (dry run)")).ToList();
                else
                {
                    string dir = Path.Combine(outDir, label.Replace('/', '_'));
                    Directory.CreateDirectory(dir);
                    bool kernel = edits.Any(e => e.File.StartsWith("kernel/", StringComparison.Ordinal)
                                              || e.File.StartsWith("abi/", StringComparison.Ordinal));
                    string? bp = TestSupport.Build(root, build, Path.Combine(dir, "build.log"), kernel);
                    if (bp is not null)
                        got = run.Select(x => new SabotageOutcome(x.Name, SabotageVerdict.BuildFailed, bp)).ToList();
                    else
                    {
                        var (boot, bootProblem) = TestSupport.Boot(root, run[0].BootArgs, dir, s.TimeoutSeconds);
                        got = boot is null
                            ? run.Select(x => new SabotageOutcome(x.Name, SabotageVerdict.Inconclusive, bootProblem!)).ToList()
                            : SabotageJudge.Judge(run, boot).ToList();
                    }
                }

                var left = guard.Restore();
                outcomes.AddRange(got);
                foreach (var o in got) Print(o, s.DryRun);
                if (left.Count > 0) return Stop($"after {label}: the restore failed", left);
                if (guard.Fingerprint() != fp0)
                    return Stop($"after {label}: the tree differs from before the run; stopping (backups kept in {state})", []);
            }

            WriteSummary(Path.Combine(outDir, "summary.json"), s.File ?? "--all", s.DryRun, outcomes);
            int caught = outcomes.Count(o => o.Verdict == SabotageVerdict.Caught);
            bool all = caught == outcomes.Count;
            AnsiConsole.MarkupLine((all ? "[green]" : "[red]") +
                $"{(s.DryRun ? "applied" : "caught")} {caught} of {outcomes.Count}[/] [grey](summary: {Markup.Escape(Path.Combine(outDir, "summary.json"))})[/]");
            return all ? 0 : 1;
        }
        finally
        {
            guard.Restore();
            Console.CancelKeyPress -= onCancel;
        }
    }

    private static void Print(SabotageOutcome o, bool dry)
    {
        string tag = o.Verdict switch
        {
            SabotageVerdict.Caught => dry ? "[green]APPLIES[/]" : "[green]CAUGHT[/]",
            SabotageVerdict.Green => "[red]GREEN[/]",
            SabotageVerdict.RedElsewhere => "[red]RED ELSEWHERE[/]",
            SabotageVerdict.Inconclusive => "[yellow]INCONCLUSIVE[/]",
            SabotageVerdict.NotApplied => "[yellow]NOT APPLIED[/]",
            _ => "[yellow]BUILD FAILED[/]",
        };
        AnsiConsole.MarkupLine($"{tag} {Markup.Escape(o.Name)} - {Markup.Escape(o.Detail)}");
    }

    private static int Stop(string why, IReadOnlyList<string> details)
    {
        AnsiConsole.MarkupLine($"[red]stopped:[/] {Markup.Escape(why)}");
        foreach (string d in details) AnsiConsole.MarkupLine($"  {Markup.Escape(d)}");
        return 2;
    }

    private static void WriteSummary(string path, string file, bool dry, IReadOnlyList<SabotageOutcome> outcomes)
    {
        using var fs = System.IO.File.Create(path);
        using var w = new Utf8JsonWriter(fs, new JsonWriterOptions
        {
            Indented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        });
        w.WriteStartObject();
        w.WriteString("file", file);
        w.WriteBoolean("dryRun", dry);
        w.WriteStartArray("sabotages");
        foreach (var o in outcomes)
        {
            w.WriteStartObject();
            w.WriteString("name", o.Name);
            w.WriteString("verdict", o.Verdict.ToString());
            w.WriteString("detail", o.Detail);
            w.WriteEndObject();
        }
        w.WriteEndArray();
        w.WriteEndObject();
    }
}
