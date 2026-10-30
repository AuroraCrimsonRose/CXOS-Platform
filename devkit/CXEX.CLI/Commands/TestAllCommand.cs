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
/// Every test there is, one after the other, into one summary (roadmap
/// 1.5d3): the OS built and booted in each variant, the DevKit's tests and
/// the kernel's host tests (<c>dotnet test</c>, read from its TRX file), then
/// the Differential suites in the CI container. In that order, never two at
/// once: a build beside a boot fails icount timing tests, and nothing may
/// touch the tree while the container has it mounted. A step that could not
/// run is SKIPPED, never a pass; the exit code is 0 only if every step ran
/// and passed, 1 if any failed, else 2.
/// </summary>
public class TestAllCommand : Command<TestAllCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandOption("--skip")]
        [Description("Steps not to run, comma-separated: boot, devkit, container. A skipped step is reported, and keeps the exit code at 2")]
        public string? Skip { get; set; }

        [CommandOption("--key")]
        [Description("The signing key os build uses. Default: test")]
        public string? Key { get; set; }

        [CommandOption("-t|--timeout")]
        [Description("Seconds each boot gets (verify-boot -t)")]
        [DefaultValue(600)]
        public int TimeoutSeconds { get; set; } = 600;

        [CommandOption("--root")]
        [Description("The repository root. Default: found from the working directory")]
        public string? Root { get; set; }
    }

    private enum Status { Pass, Fail, Skipped }
    private sealed record Step(string Name, Status Status, string Detail);

    /// <summary>The boot variants: what a kernel change has to pass (CLAUDE.md, "Build and test").</summary>
    private static readonly (string Name, string[] Args)[] Variants =
    [
        ("boot", []),
        ("boot --cpu max", ["--cpu", "max"]),
    ];

    protected override int Execute(CommandContext context, Settings s, CancellationToken cancellationToken)
    {
        string? root = OsBuildCommand.ResolveRoot(s.Root);
        if (root is null) { AnsiConsole.MarkupLine("[red]error:[/] could not find the repository root (--root)"); return 2; }
        var skip = (s.Skip ?? "").Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).ToHashSet();
        var bad = skip.Where(x => x is not ("boot" or "devkit" or "container")).ToList();
        if (bad.Count > 0) { AnsiConsole.MarkupLine($"[red]error:[/] --skip takes boot, devkit, container, not {Markup.Escape(string.Join(", ", bad))}"); return 2; }

        string dir = Path.Combine(root, "build", "test-all");
        Directory.CreateDirectory(dir);
        var steps = new List<Step>();
        void Add(Step st)
        {
            steps.Add(st);
            string tag = st.Status switch { Status.Pass => "[green]PASS[/]", Status.Fail => "[red]FAIL[/]", _ => "[yellow]SKIPPED[/]" };
            AnsiConsole.MarkupLine($"{tag} {Markup.Escape(st.Name)} - {Markup.Escape(st.Detail)}");
        }

        if (skip.Contains("boot"))
            foreach (var v in Variants) Add(new Step(v.Name, Status.Skipped, "--skip boot"));
        else
        {
            AnsiConsole.MarkupLine("[grey]building the OS[/]");
            string? bp = TestSupport.Build(root, ["--key", s.Key ?? "test"], Path.Combine(dir, "os-build.log"), kernelChanged: true);
            foreach (var v in Variants)
            {
                if (bp is not null) { Add(new Step(v.Name, Status.Fail, bp)); continue; }
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(v.Name)}[/]");
                var (boot, problem) = TestSupport.Boot(root, v.Args, Path.Combine(dir, v.Name.Replace(' ', '_').Replace("--", "")), s.TimeoutSeconds);
                if (boot is null) Add(new Step(v.Name, Status.Fail, problem!));
                else
                {
                    int failed = boot.Results.Count(r => !r.Passed);
                    Add(new Step(v.Name, boot.Outcome == "pass" ? Status.Pass : Status.Fail,
                        $"{boot.Outcome}, {boot.Results.Count - failed} of {boot.Results.Count} self-tests: {boot.Reason}"));
                }
            }
        }

        if (skip.Contains("devkit"))
        {
            Add(new Step("devkit", Status.Skipped, "--skip devkit"));
            Add(new Step("host", Status.Skipped, "--skip devkit (the host tests run inside dotnet test)"));
        }
        else
        {
            AnsiConsole.MarkupLine("[grey]dotnet test (DevKit and kernel host tests)[/]");
            string trx = Path.Combine(dir, "devkit.trx");
            if (File.Exists(trx)) File.Delete(trx);
            var r = ToolProcess.Run("dotnet", ["test", Path.Combine(root, "devkit", "CXEX.slnx"), "-c", "Release",
                                               "--logger", "trx;LogFileName=devkit.trx", "--results-directory", dir],
                                    new ToolRunOptions { WorkingDirectory = root, Timeout = TimeSpan.FromMinutes(40) });
            File.WriteAllText(Path.Combine(dir, "devkit.log"), r.StandardOutput + "\n--- stderr ---\n" + r.StandardError);
            var report = File.Exists(trx) ? TrxReport.Parse(File.ReadAllText(trx)) : null;
            if (report is null || report.Tests.Count == 0)
            {
                string why = $"no test results ({r.Describe()}; log: {Path.Combine(dir, "devkit.log")})";
                Add(new Step("devkit", Status.Fail, why));
                Add(new Step("host", Status.Fail, why));
            }
            else
            {
                static bool IsHost(TrxTest t) => t.Name.Contains("KernelHostTests", StringComparison.Ordinal);
                var devkit = report.Where(t => !IsHost(t));
                var host = report.Where(IsHost);
                Add(new Step("devkit", devkit.Failed == 0 && devkit.Passed > 0 ? Status.Pass : Status.Fail,
                    $"{devkit.Passed} passed, {devkit.Failed} failed, {devkit.Skipped} skipped" +
                    (devkit.Failed > 0 ? ": " + string.Join("; ", devkit.Tests.Where(t => t.Outcome == "Failed").Take(5).Select(t => t.Name)) : "")));
                Add(host.Tests.Count == 0 ? new Step("host", Status.Fail, "the kernel host test is missing from the results")
                    : host.Failed > 0 ? new Step("host", Status.Fail, "the kernel host tests failed (detail in devkit.trx)")
                    : host.Skipped > 0 ? new Step("host", Status.Skipped, "the kernel host test skipped (a missing toolchain; devkit.trx says which)")
                    : new Step("host", Status.Pass, "the kernel host tests passed under ASan and UBSan"));
            }
        }

        if (skip.Contains("container")) Add(new Step("container", Status.Skipped, "--skip container"));
        else Add(Container(root, dir));

        WriteSummary(Path.Combine(dir, "summary.json"), steps);
        int code = steps.Any(x => x.Status == Status.Fail) ? 1 : steps.Any(x => x.Status == Status.Skipped) ? 2 : 0;
        AnsiConsole.MarkupLine((code == 0 ? "[green]" : code == 1 ? "[red]" : "[yellow]") +
            $"{steps.Count(x => x.Status == Status.Pass)} of {steps.Count} steps passed[/] [grey](summary: {Markup.Escape(Path.Combine(dir, "summary.json"))})[/]");
        return code;
    }

    /// <summary>The Differential suites in the cxos-ci image, as HANDOFF runs them: build first, in the same run.</summary>
    private static Step Container(string root, string dir)
    {
        var probe = ToolProcess.Run("docker", ["image", "inspect", "cxos-ci", "--format", "{{.Id}}"],
                                    new ToolRunOptions { Timeout = TimeSpan.FromMinutes(1) });
        if (!probe.Ok)
            return new Step("container", Status.Skipped,
                probe.Outcome == ToolOutcome.NotFound ? "docker is not installed" : "docker is not running, or has no cxos-ci image (tools/ci/Dockerfile)");

        AnsiConsole.MarkupLine("[grey]the container's Differential run (touch nothing in the tree until it ends)[/]");
        string trx = Path.Combine(dir, "container.trx");
        if (File.Exists(trx)) File.Delete(trx);
        var r = ToolProcess.Run("docker", ["run", "--rm",
            "--mount", $"type=bind,src={root},dst=/src",
            // keeps out the Windows CMake cache
            "--mount", "type=tmpfs,dst=/src/build/hosttest",
            "-w", "/src", "cxos-ci", "bash", "-c",
            "dotnet build devkit/CXEX.slnx -c Release -v q && dotnet test devkit/CXEX.Tests -c Release --no-build " +
            "--filter \"Category=Differential\" --logger \"trx;LogFileName=container.trx\" --results-directory /src/build/test-all"],
            new ToolRunOptions { WorkingDirectory = root, Timeout = TimeSpan.FromMinutes(60) });
        File.WriteAllText(Path.Combine(dir, "container.log"), r.StandardOutput + "\n--- stderr ---\n" + r.StandardError);
        var report = File.Exists(trx) ? TrxReport.Parse(File.ReadAllText(trx)) : null;
        if (report is null || report.Tests.Count == 0)
            return new Step("container", Status.Fail, $"no test results ({r.Describe()}; log: {Path.Combine(dir, "container.log")})");
        var status = report.Failed == 0 && report.Skipped == 0 && report.Passed > 0 ? Status.Pass : Status.Fail;
        return new Step("container", status, $"Differential: {report.Passed} passed, {report.Failed} failed, {report.Skipped} skipped" +
            (report.Failed > 0 ? ": " + string.Join("; ", report.Tests.Where(t => t.Outcome == "Failed").Take(5).Select(t => t.Name)) : ""));
    }

    private static void WriteSummary(string path, IReadOnlyList<Step> steps)
    {
        using var fs = File.Create(path);
        using var w = new Utf8JsonWriter(fs, new JsonWriterOptions
        {
            Indented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        });
        w.WriteStartObject();
        w.WriteStartArray("steps");
        foreach (var st in steps)
        {
            w.WriteStartObject();
            w.WriteString("name", st.Name);
            w.WriteString("status", st.Status.ToString().ToLowerInvariant());
            w.WriteString("detail", st.Detail);
            w.WriteEndObject();
        }
        w.WriteEndArray();
        w.WriteEndObject();
    }
}
