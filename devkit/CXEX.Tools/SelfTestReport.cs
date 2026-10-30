// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Globalization;
using System.Text.RegularExpressions;

namespace CXEX.Tools;

/// <summary>One self-test's verdict, from its <c>@ktest result</c> line.</summary>
/// <param name="Index">Its place in the run, from 0.</param>
/// <param name="Name">The name <c>ktest_run</c> reports it under.</param>
/// <param name="Passed">Whether it passed.</param>
/// <param name="Ms">Virtual milliseconds (icount) since the line before it.</param>
/// <param name="Detail">For a failed test, the error and failure lines the
/// kernel logged while it ran (its "failed at step N"), prefix stripped.</param>
public sealed record SelfTestResult(int Index, string Name, bool Passed, long Ms, IReadOnlyList<string> Detail);

/// <summary>
/// The kernel's per-test lines (kernel/ktest.c, roadmap 1.5d): one
/// <c>@ktest begin</c>, a <c>@ktest result</c> per self-test and one
/// <c>@ktest end</c>, tab-separated, on serial only. A line is matched
/// anywhere it starts, not only at column 0: the kernel writes it whole with
/// interrupts off, but it may land after half of another thread's line.
/// </summary>
public sealed record SelfTestReport(bool Begun, IReadOnlyList<SelfTestResult> Results, int? EndPassed, int? EndTotal)
{
    private static readonly Regex Tagged = new(@"@ktest\t([^\r\n]*)");
    // "[KTEST   ] - [ERR ] - message": a line the kernel logged as a failure.
    private static readonly Regex Problem = new(@"\[[^\]]*\] - \[(ERR |FAIL)\] - (.*)$");

    public int Failed => Results.Count(r => !r.Passed);

    public static SelfTestReport Parse(string log)
    {
        bool begun = false;
        int? endPassed = null, endTotal = null;
        var results = new List<SelfTestResult>();
        var pending = new List<string>();

        foreach (string raw in log.Split('\n'))
        {
            string line = raw.TrimEnd('\r');
            var m = Tagged.Match(line);
            if (!m.Success)
            {
                var p = Problem.Match(line);
                if (p.Success) pending.Add(p.Groups[2].Value.Trim());
                continue;
            }

            string[] f = m.Groups[1].Value.Split('\t');
            switch (f[0])
            {
                case "begin":
                    begun = true;
                    results.Clear();
                    endPassed = endTotal = null;
                    break;
                case "result" when f.Length >= 5 && Int(f[1]) is int idx && Int(f[3]) is int ms
                                   && (f[2] == "pass" || f[2] == "fail"):
                    bool passed = f[2] == "pass";
                    string name = string.Join('\t', f[4..]);
                    // the summary's own FAIL line names the test: not detail
                    IReadOnlyList<string> detail = passed ? [] : pending.Where(d => d != name).ToList();
                    results.Add(new SelfTestResult(idx, name, passed, ms, detail));
                    break;
                case "end" when f.Length >= 3:
                    endPassed = Int(f[1]);
                    endTotal = Int(f[2]);
                    break;
            }
            pending.Clear();
        }
        return new SelfTestReport(begun, results, endPassed, endTotal);
    }

    /// <summary>
    /// Whether these lines agree with the summary's "all <paramref name="n"/>
    /// passed": a begin, then n results numbered 0 to n-1, each a pass, and an
    /// end of n of n. Null if they do; otherwise what is wrong.
    /// </summary>
    public string? DisagreesWith(int n)
    {
        if (!Begun) return "no @ktest lines: the per-test results are missing";
        if (EndTotal is null || EndPassed is null) return "the @ktest results have no end line";
        if (EndTotal != n || EndPassed != n)
            return $"the @ktest end line says {EndPassed} of {EndTotal}, the summary all {n}";
        if (Results.Count != n) return $"{Results.Count} @ktest results for {n} self-tests";
        for (int i = 0; i < Results.Count; i++)
            if (Results[i].Index != i) return $"@ktest result {i} is numbered {Results[i].Index}";
        var bad = Results.FirstOrDefault(r => !r.Passed);
        if (bad is not null) return $"@ktest result {bad.Index} ({bad.Name}) failed";
        return null;
    }

    private static int? Int(string s) =>
        int.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, out int v) ? v : null;

    /// <summary>
    /// A boot's verdict and its self-tests as one JSON object, for
    /// <c>cxk verify-boot --json</c> and what reads it (<c>cxk test all</c>):
    /// <c>outcome</c> is pass, fail or inconclusive and <c>exitCode</c> the
    /// command's (0, 1, 2); <c>selfTests.results</c> holds every test, a failed
    /// one with the lines it logged.
    /// </summary>
    public static string Json(BootVerdict verdict, SelfTestReport tests, string image, string log, bool full, string? cpu)
    {
        using var ms = new MemoryStream();
        using (var w = new System.Text.Json.Utf8JsonWriter(ms, new System.Text.Json.JsonWriterOptions
        {
            Indented = true,
            // test names and log lines read as written; nothing here goes into HTML
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        }))
        {
            w.WriteStartObject();
            w.WriteString("image", image);
            w.WriteString("log", log);
            w.WriteBoolean("full", full);
            if (cpu is null) w.WriteNull("cpu"); else w.WriteString("cpu", cpu);
            w.WriteString("outcome", verdict.Outcome.ToString().ToLowerInvariant());
            w.WriteNumber("exitCode", (int)verdict.Outcome);
            w.WriteString("reason", verdict.Reason);
            w.WriteStartObject("selfTests");
            w.WriteBoolean("begun", tests.Begun);
            w.WriteBoolean("ended", tests.EndTotal is not null);
            w.WriteNumber("total", tests.Results.Count);
            w.WriteNumber("passed", tests.Results.Count - tests.Failed);
            w.WriteNumber("failed", tests.Failed);
            w.WriteStartArray("results");
            foreach (var r in tests.Results)
            {
                w.WriteStartObject();
                w.WriteNumber("index", r.Index);
                w.WriteString("name", r.Name);
                w.WriteBoolean("passed", r.Passed);
                w.WriteNumber("ms", r.Ms);
                w.WriteStartArray("detail");
                foreach (string d in r.Detail) w.WriteStringValue(d);
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
            w.WriteEndObject();
        }
        return System.Text.Encoding.UTF8.GetString(ms.ToArray());
    }
}
