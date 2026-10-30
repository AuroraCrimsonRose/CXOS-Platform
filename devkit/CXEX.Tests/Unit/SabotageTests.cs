// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// <c>cxk test sabotage</c>'s parts (roadmap 1.5d3): the sabotage file, the
/// judge, and the backup and restore. Each case is a trap a hand-written
/// matrix script fell into (memory: mistakes-not-to-repeat).
/// </summary>
public class SabotageTests
{
    private const string Two = """
        # two sabotages in one file
        [owner]
        expect = x: sync: a mutex's word is its owner @ step 21
        group = a
        edit = os/std/sync.xfxn
        <<<
            if (a || b) { x = ~y; }   # C's || and ~ and a hash, as written
        ===
            if (0) { x = ~y; }
        >>>

        [wake]
        expect = hang
        edit = kernel/a.c
        <<<
        one
        two
        ===
        >>>
        """;

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_file_reads_with_its_text_taken_as_written()
    {
        var all = SabotageFile.Parse(Two);
        Assert.Equal(2, all.Count);
        Assert.Equal("    if (a || b) { x = ~y; }   # C's || and ~ and a hash, as written", all[0].Edits[0].Old);
        Assert.Equal("x: sync: a mutex's word is its owner", all[0].Expects[0].Name);
        Assert.Equal("step 21", all[0].Expects[0].Detail);
        Assert.Equal("a", all[0].Group);
        Assert.True(all[1].Hangs);
        Assert.Equal("one\ntwo", all[1].Edits[0].Old);
        Assert.Equal("", all[1].Edits[0].New);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("[a]\nedit = f\n<<<\nx\n===\ny\n>>>\n", "has no expect")]
    [InlineData("[a]\nexpect = t\n", "has no edit")]
    [InlineData("[a]\nexpect = hang\ngroup = g\nedit = f\n<<<\nx\n===\ny\n>>>\n", "booted alone")]
    [InlineData("[a]\nexpect = t\nedit = ../x\n<<<\nx\n===\ny\n>>>\n", "inside the tree")]
    [InlineData("[a]\nexpect = t\nedit = f\n<<<\nx\n===\nx\n>>>\n", "changes nothing")]
    [InlineData("[a]\nexpect = t\nedit = f\n<<<\nx\n", "no '==='")]
    [InlineData("[a]\nexpect = t\nedit = f\n<<<\nx\n===\ny\n>>>\n[a]\n", "named twice")]
    [InlineData("[a]\nexpect = t\nwhat = 1\n", "unknown key")]
    public void A_malformed_file_is_refused_with_its_line(string text, string message)
    {
        var e = Assert.Throws<FormatException>(() => SabotageFile.Parse(text));
        Assert.Contains(message, e.Message);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_edit_applies_only_to_text_that_occurs_once()
    {
        var e = new SabotageEdit("f", "check();", "0;", 1);
        Assert.Equal("a 0; b", SabotageFile.Apply("a check(); b", e).Result);
        // 1.3b: the pattern also matched an older function's identical check
        Assert.Contains("occurs 2 times", SabotageFile.Apply("check(); check();", e).Problem);
        Assert.Contains("occurs 0 times", SabotageFile.Apply("nothing", e).Problem);
        Assert.Contains("CR line endings", SabotageFile.Apply("check();\r\n", e).Problem);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Groups_become_one_run_and_others_run_alone()
    {
        string text = "[a]\nexpect = t\ngroup = g\nedit = f\n<<<\nx\n===\ny\n>>>\n" +
                      "[b]\nexpect = t\nedit = f\n<<<\nx\n===\ny\n>>>\n" +
                      "[c]\nexpect = u\ngroup = g\nedit = h\n<<<\nx\n===\ny\n>>>\n";
        var runs = SabotageFile.Runs(SabotageFile.Parse(text));
        Assert.Equal(["a+c", "b"], runs.Select(r => string.Join("+", r.Select(s => s.Name))));
    }

    private static BootTestResult R(int i, string name, bool passed, params string[] detail) => new(i, name, passed, detail);
    private static Sabotage S(string name, params SabotageExpect[] expects) =>
        new(name, [new SabotageEdit("f", "x", "y", 1)], expects, false, null, [], 1);
    private static SabotageExpect E(string name, string? detail = null, bool prefix = false) => new(name, prefix, detail);

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_sabotage_is_caught_only_where_it_said()
    {
        var boot = new BootJson("fail", "self-tests failed", [R(0, "a", true), R(1, "owner", false, "failed at step 21")]);
        Assert.Equal(SabotageVerdict.Caught, SabotageJudge.Judge([S("s", E("owner", "step 21"))], boot)[0].Verdict);
        // the right test, the wrong step (1.1d: red for the wrong reason)
        Assert.Equal(SabotageVerdict.RedElsewhere, SabotageJudge.Judge([S("s", E("owner", "step 22"))], boot)[0].Verdict);
        Assert.Equal(SabotageVerdict.RedElsewhere, SabotageJudge.Judge([S("s", E("other"))], boot)[0].Verdict);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_red_nothing_in_the_run_names_fails_every_sabotage_in_it()
    {
        var boot = new BootJson("fail", "", [R(0, "a", false), R(1, "b", false), R(2, "c", false)]);
        var got = SabotageJudge.Judge([S("s", E("a")), S("t", E("b"))], boot);
        Assert.All(got, o => Assert.Equal(SabotageVerdict.RedElsewhere, o.Verdict));
        Assert.Contains("also red at c", got[0].Detail);
        // a prefix explains every test it matches ("not run" after a hang)
        var withPrefix = SabotageJudge.Judge([S("s", E("a")), S("t", E("", prefix: true))], boot);
        Assert.All(withPrefix, o => Assert.Equal(SabotageVerdict.Caught, o.Verdict));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_green_or_silent_boot_is_never_caught()
    {
        var s = S("s", E("a"));
        Assert.Equal(SabotageVerdict.Green, SabotageJudge.Judge([s], new BootJson("pass", "", [R(0, "a", true)]))[0].Verdict);
        Assert.Equal(SabotageVerdict.Inconclusive, SabotageJudge.Judge([s], new BootJson("inconclusive", "timed out", []))[0].Verdict);
        // a panic with every self-test passed is red, but at no test it named
        Assert.Equal(SabotageVerdict.RedElsewhere, SabotageJudge.Judge([s], new BootJson("fail", "panic", [R(0, "a", true)]))[0].Verdict);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_hang_sabotage_is_caught_by_a_hang_only()
    {
        var s = new Sabotage("h", [new SabotageEdit("f", "x", "y", 1)], [], true, null, [], 1);
        Assert.Equal(SabotageVerdict.Caught, SabotageJudge.Judge([s], new BootJson("inconclusive", "timed out", []))[0].Verdict);
        Assert.Equal(SabotageVerdict.Green, SabotageJudge.Judge([s], new BootJson("pass", "", []))[0].Verdict);
        Assert.Equal(SabotageVerdict.RedElsewhere, SabotageJudge.Judge([s], new BootJson("fail", "", [R(0, "a", false)]))[0].Verdict);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Verify_boot_json_reads_back()
    {
        string json = SelfTestReport.Json(new BootVerdict(BootOutcome.Fail, "1 failed", 0),
            SelfTestReport.Parse("@ktest\tbegin\n[KTEST   ] - [ERR ] - failed at step 3\n@ktest\tresult\t0\tfail\t5\tx: one\n@ktest\tend\t0\t1\n"),
            "img", "log", true, null);
        var b = BootJson.Parse(json)!;
        Assert.Equal("fail", b.Outcome);
        Assert.Equal("x: one", b.Results[0].Name);
        Assert.Equal(["failed at step 3"], b.Results[0].Detail);
        Assert.Null(BootJson.Parse("not json"));
    }

    private static string Scratch()
    {
        string d = Path.Combine(Path.GetTempPath(), "cxk-sabotage-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(d);
        return d;
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Two_edits_to_one_file_restore_the_original()
    {
        // 2026-10-28 and 1.3e: a backup per edit restored the half-sabotaged copy
        string repo = Scratch();
        try
        {
            string f = Path.Combine(repo, "a.c");
            File.WriteAllText(f, "one\ntwo\n");
            var g = new TreeGuard(repo, Path.Combine(repo, "state"));
            Assert.Null(g.Apply([new SabotageEdit("a.c", "one", "ONE", 1), new SabotageEdit("a.c", "two", "TWO", 2)]));
            Assert.Equal("ONE\nTWO\n", File.ReadAllText(f));
            Assert.Empty(g.Restore());
            Assert.Equal("one\ntwo\n", File.ReadAllText(f));
            Assert.False(g.Pending);
        }
        finally { Directory.Delete(repo, true); }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_run_that_died_is_restored_by_the_next()
    {
        string repo = Scratch();
        try
        {
            string f = Path.Combine(repo, "a.c");
            File.WriteAllText(f, "one\n");
            Assert.Null(new TreeGuard(repo, Path.Combine(repo, "state")).Apply([new SabotageEdit("a.c", "one", "ONE", 1)]));
            // that guard is gone without restoring; a new one finds its manifest
            var next = new TreeGuard(repo, Path.Combine(repo, "state"));
            Assert.True(next.Pending);
            Assert.Empty(next.Restore());
            Assert.Equal("one\n", File.ReadAllText(f));
        }
        finally { Directory.Delete(repo, true); }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_edit_that_does_not_apply_leaves_the_run_restored()
    {
        string repo = Scratch();
        try
        {
            File.WriteAllText(Path.Combine(repo, "a.c"), "one\n");
            File.WriteAllText(Path.Combine(repo, "b.c"), "x x\n");
            var g = new TreeGuard(repo, Path.Combine(repo, "state"));
            string? p = g.Apply([new SabotageEdit("a.c", "one", "ONE", 1), new SabotageEdit("b.c", "x", "y", 2)]);
            Assert.Contains("occurs 2 times", p);
            Assert.Equal("one\n", File.ReadAllText(Path.Combine(repo, "a.c")));
            Assert.False(g.Pending);
        }
        finally { Directory.Delete(repo, true); }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_trx_file_is_counted_by_outcome()
    {
        const string trx = """
            <?xml version="1.0" encoding="utf-8"?>
            <TestRun xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010"><Results>
              <UnitTestResult testName="A.one" outcome="Passed" />
              <UnitTestResult testName="A.two" outcome="Failed" />
              <UnitTestResult testName="KernelHostTests.k" outcome="NotExecuted" />
            </Results></TestRun>
            """;
        var r = TrxReport.Parse(trx)!;
        Assert.Equal((1, 1, 1), (r.Passed, r.Failed, r.Skipped));
        Assert.Single(r.Where(t => t.Name.Contains("KernelHostTests")).Tests);
        Assert.Null(TrxReport.Parse("<unclosed"));
    }
}
