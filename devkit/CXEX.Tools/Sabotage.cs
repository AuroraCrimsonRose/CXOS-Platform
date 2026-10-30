// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.Json;

namespace CXEX.Tools;

/// <summary>One text replacement: <paramref name="Old"/> must occur exactly once in the file.</summary>
public sealed record SabotageEdit(string File, string Old, string New, int Line);

/// <summary>
/// Where a sabotage must turn a boot red: a self-test whose name is
/// <paramref name="Name"/> (or starts with it, when <paramref name="Prefix"/>),
/// and, when <paramref name="Detail"/> is set, one of whose logged lines
/// contains it ("step 21").
/// </summary>
public sealed record SabotageExpect(string Name, bool Prefix, string? Detail)
{
    public bool Matches(BootTestResult r) =>
        !r.Passed
        && (Prefix ? r.Name.StartsWith(Name, StringComparison.Ordinal) : r.Name == Name)
        && (Detail is null || r.Detail.Any(d => d.Contains(Detail, StringComparison.Ordinal)));

    public override string ToString() => Name + (Prefix ? "*" : "") + (Detail is null ? "" : " @ " + Detail);
}

/// <summary>
/// One deliberate break of the tree and where it must be caught
/// (roadmap 1.5d3, safety/CX_SAFETY.md §7). <paramref name="Hangs"/>: the
/// sabotage removes a wakeup or a deadlock refusal, so a caught boot hangs
/// (INCONCLUSIVE) instead of failing; it is booted alone.
/// </summary>
public sealed record Sabotage(string Name, IReadOnlyList<SabotageEdit> Edits, IReadOnlyList<SabotageExpect> Expects,
                              bool Hangs, string? Group, IReadOnlyList<string> BootArgs, int Line);

/// <summary>A self-test's result as <c>cxk verify-boot --json</c> prints it.</summary>
public sealed record BootTestResult(int Index, string Name, bool Passed, IReadOnlyList<string> Detail);

/// <summary>What <c>cxk verify-boot --json</c> printed: its outcome (pass, fail, inconclusive), reason and results.</summary>
public sealed record BootJson(string Outcome, string Reason, IReadOnlyList<BootTestResult> Results)
{
    public static BootJson? Parse(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var results = new List<BootTestResult>();
            if (root.TryGetProperty("selfTests", out var st) && st.TryGetProperty("results", out var rs))
                foreach (var r in rs.EnumerateArray())
                    results.Add(new BootTestResult(
                        r.GetProperty("index").GetInt32(),
                        r.GetProperty("name").GetString() ?? "",
                        r.GetProperty("passed").GetBoolean(),
                        r.GetProperty("detail").EnumerateArray().Select(d => d.GetString() ?? "").ToList()));
            return new BootJson(root.GetProperty("outcome").GetString() ?? "",
                                root.GetProperty("reason").GetString() ?? "", results);
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            return null;
        }
    }
}

/// <summary>
/// Reads a sabotage file (<c>tests/sabotage/*.sab</c>; the format is in
/// docs/devkit/CX_CXK_PLAN.md, "cxk test sabotage"):
/// <code>
/// # a comment
/// [name]
/// expect = x: sync: a mutex's word is its owner @ step 21
/// expect = x: *            (a trailing * matches a prefix)
/// expect = hang            (caught means the boot hangs; booted alone)
/// group = a                (shares one build and boot with the rest of group a)
/// boot = --cpu max         (more verify-boot arguments)
/// edit = os/std/sync.xfxn
/// &lt;&lt;&lt;
/// the text to replace, exactly, every line
/// ===
/// what replaces it
/// &gt;&gt;&gt;
/// </code>
/// The text between the markers is taken as written, with no escapes, so C's
/// <c>||</c>, <c>~</c> and <c>#</c> need nothing (each was a delimiter some
/// hand-written matrix tripped on).
/// </summary>
public static class SabotageFile
{
    public static IReadOnlyList<Sabotage> Parse(string text, string source = "sabotage file")
    {
        var lines = text.Replace("\r\n", "\n").Split('\n');
        var list = new List<Sabotage>();
        string? name = null;
        int nameLine = 0;
        var edits = new List<SabotageEdit>();
        var expects = new List<SabotageExpect>();
        var boot = new List<string>();
        bool hangs = false;
        string? group = null;

        void Fail(int line, string msg) => throw new FormatException($"{source}:{line}: {msg}");

        void Finish()
        {
            if (name is null) return;
            if (edits.Count == 0) Fail(nameLine, $"[{name}] has no edit");
            if (expects.Count == 0 && !hangs)
                Fail(nameLine, $"[{name}] has no expect: say which self-test must go red");
            if (hangs && expects.Count > 0) Fail(nameLine, $"[{name}] expects a hang and a red test; a hang has no results to name");
            if (hangs && group is not null) Fail(nameLine, $"[{name}] expects a hang: it is booted alone, never in a group");
            list.Add(new Sabotage(name, edits.ToList(), expects.ToList(), hangs, group, boot.ToList(), nameLine));
            name = null;
        }

        for (int i = 0; i < lines.Length; i++)
        {
            int ln = i + 1;
            string line = lines[i].Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                Finish();
                name = line[1..^1].Trim();
                if (name.Length == 0 || !name.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.'))
                    Fail(ln, $"a sabotage name is letters, digits, '-', '_' and '.', not '{name}'");
                if (list.Any(s => s.Name == name)) Fail(ln, $"[{name}] is named twice");
                nameLine = ln;
                edits.Clear(); expects.Clear(); boot.Clear();
                hangs = false; group = null;
                continue;
            }

            int eq = line.IndexOf('=');
            if (eq < 0) Fail(ln, $"expected 'key = value', '[name]' or a comment, not '{line}'");
            if (name is null) Fail(ln, "a key before the first [name]");
            string key = line[..eq].Trim(), value = line[(eq + 1)..].Trim();
            switch (key)
            {
                case "expect" when value == "hang":
                    hangs = true;
                    break;
                case "expect":
                {
                    string? detail = null;
                    int at = value.IndexOf(" @ ", StringComparison.Ordinal);
                    if (at >= 0) { detail = value[(at + 3)..].Trim(); value = value[..at].Trim(); }
                    bool prefix = value.EndsWith('*');
                    if (prefix) value = value[..^1];
                    if (value.Length == 0 && !prefix) Fail(ln, "expect names no test");
                    expects.Add(new SabotageExpect(value, prefix, detail));
                    break;
                }
                case "group":
                    group = value;
                    break;
                case "boot":
                    boot.AddRange(value.Split(' ', StringSplitOptions.RemoveEmptyEntries));
                    break;
                case "edit":
                {
                    if (value.Length == 0 || Path.IsPathRooted(value) || value.Split('/', '\\').Contains(".."))
                        Fail(ln, $"an edit names a file inside the tree, relative to it, not '{value}'");
                    if (i + 1 >= lines.Length || lines[i + 1] != "<<<") Fail(ln + 1, "an edit's text starts with a line '<<<'");
                    int j = i + 2;
                    var old = new List<string>();
                    while (j < lines.Length && lines[j] != "===") old.Add(lines[j++]);
                    if (j >= lines.Length) Fail(ln, "an edit's text has no '===' line");
                    j++;
                    var neu = new List<string>();
                    while (j < lines.Length && lines[j] != ">>>") neu.Add(lines[j++]);
                    if (j >= lines.Length) Fail(ln, "an edit's text has no '>>>' line");
                    if (old.Count == 0 || string.Join("\n", old).Trim().Length == 0) Fail(ln, "an edit replaces nothing");
                    string o = string.Join("\n", old), n = string.Join("\n", neu);
                    if (o == n) Fail(ln, "an edit that changes nothing");
                    edits.Add(new SabotageEdit(value.Replace('\\', '/'), o, n, ln));
                    i = j;
                    break;
                }
                default:
                    Fail(ln, $"unknown key '{key}' (expect, group, boot, edit)");
                    break;
            }
        }
        Finish();
        if (list.Count == 0) throw new FormatException($"{source}: no sabotages");
        return list;
    }

    /// <summary>
    /// The runs: a sabotage with no group alone, a group's sabotages together,
    /// in the order the file first names each. A group's boot arguments must agree.
    /// </summary>
    public static IReadOnlyList<IReadOnlyList<Sabotage>> Runs(IReadOnlyList<Sabotage> all)
    {
        var runs = new List<List<Sabotage>>();
        var byGroup = new Dictionary<string, List<Sabotage>>();
        foreach (var s in all)
        {
            if (s.Group is null) { runs.Add([s]); continue; }
            if (!byGroup.TryGetValue(s.Group, out var g))
            {
                byGroup[s.Group] = g = [];
                runs.Add(g);
            }
            else if (!g[0].BootArgs.SequenceEqual(s.BootArgs))
                throw new FormatException($"[{s.Name}] and [{g[0].Name}] share group {s.Group} but boot differently");
            g.Add(s);
        }
        return runs;
    }

    /// <summary>
    /// <paramref name="content"/> with <paramref name="edit"/> applied, or why
    /// it cannot be: the old text must occur exactly once (a second match is
    /// how a pattern lands on an older function's identical check), and the
    /// file must be LF, as the tree is.
    /// </summary>
    public static (string? Result, string? Problem) Apply(string content, SabotageEdit edit)
    {
        if (content.Contains('\r')) return (null, $"{edit.File} has CR line endings; the tree is LF");
        int n = 0;
        for (int at = content.IndexOf(edit.Old, StringComparison.Ordinal); at >= 0;
             at = content.IndexOf(edit.Old, at + 1, StringComparison.Ordinal))
            n++;
        if (n != 1) return (null, $"the text at line {edit.Line} occurs {n} times in {edit.File}, not once");
        int i = content.IndexOf(edit.Old, StringComparison.Ordinal);
        return (content[..i] + edit.New + content[(i + edit.Old.Length)..], null);
    }
}

/// <summary>What became of one sabotage.</summary>
public enum SabotageVerdict
{
    /// <summary>Red where it said, and nowhere it did not explain (or hung, when it said it would).</summary>
    Caught,
    /// <summary>The boot passed: nothing covers what was broken.</summary>
    Green,
    /// <summary>Red, but not at every test it named, or also at tests nothing in the run named.</summary>
    RedElsewhere,
    /// <summary>The boot gave no verdict (a hang) when a red was expected, or the reverse.</summary>
    Inconclusive,
    /// <summary>An edit could not be applied: not a result.</summary>
    NotApplied,
    /// <summary>The build failed, or left an old image: not a result.</summary>
    BuildFailed,
}

public sealed record SabotageOutcome(string Name, SabotageVerdict Verdict, string Detail);

/// <summary>Judges a boot of a run against what its sabotages expect.</summary>
public static class SabotageJudge
{
    public static IReadOnlyList<SabotageOutcome> Judge(IReadOnlyList<Sabotage> run, BootJson boot)
    {
        var red = boot.Results.Where(r => !r.Passed).ToList();
        var outcomes = new List<SabotageOutcome>();

        if (run.Count == 1 && run[0].Hangs)
        {
            var s = run[0];
            return [boot.Outcome switch
            {
                "inconclusive" => new SabotageOutcome(s.Name, SabotageVerdict.Caught, "hung, as expected: " + boot.Reason),
                "pass" => new SabotageOutcome(s.Name, SabotageVerdict.Green, "the boot passed"),
                _ => new SabotageOutcome(s.Name, SabotageVerdict.RedElsewhere,
                                         "red, not hung: " + Names(red, boot.Reason)),
            }];
        }

        if (boot.Outcome == "pass")
            return run.Select(s => new SabotageOutcome(s.Name, SabotageVerdict.Green, "the boot passed")).ToList();
        if (boot.Outcome != "fail")
            return run.Select(s => new SabotageOutcome(s.Name, SabotageVerdict.Inconclusive, boot.Reason)).ToList();

        // A red no expectation in the run names is unexplained: a sabotage
        // may be red only for its own reason (1.1d: a refusal sabotaged into
        // "refuse everything" went red at a later step and looked like a pass).
        var unexplained = red.Where(r => !run.SelectMany(s => s.Expects).Any(e => e.Matches(r))).ToList();
        foreach (var s in run)
        {
            var missing = s.Expects.Where(e => !red.Any(e.Matches)).ToList();
            if (red.Count == 0)
                outcomes.Add(new(s.Name, SabotageVerdict.RedElsewhere, "failed with every self-test passing: " + boot.Reason));
            else if (missing.Count > 0)
                outcomes.Add(new(s.Name, SabotageVerdict.RedElsewhere,
                    "not red at " + string.Join("; ", missing) + " - red: " + Names(red, boot.Reason)));
            else if (unexplained.Count > 0)
                outcomes.Add(new(s.Name, SabotageVerdict.RedElsewhere,
                    "also red at " + Names(unexplained, boot.Reason)));
            else
                outcomes.Add(new(s.Name, SabotageVerdict.Caught,
                    "red at " + Names(red.Where(r => s.Expects.Any(e => e.Matches(r))).ToList(), boot.Reason)));
        }
        return outcomes;
    }

    private static string Names(IReadOnlyList<BootTestResult> rs, string reason) =>
        rs.Count == 0 ? reason
        : string.Join("; ", rs.Take(6).Select(r => r.Detail.Count > 0 ? $"{r.Name} ({r.Detail[0]})" : r.Name))
          + (rs.Count > 6 ? $"; and {rs.Count - 6} more" : "");
}
