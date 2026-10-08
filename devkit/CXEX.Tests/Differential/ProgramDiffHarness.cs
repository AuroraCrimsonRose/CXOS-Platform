// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.RegularExpressions;
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// Compares a dump program written in X against the matching `cxk` command on
/// whole, compilable programs - every X file in CXK as its build compiles it -
/// and on mutants of them. Port of <c>devkit/tests/xc/progdiff.py</c>
/// (HARDENING_PLAN D1).
///
/// <para>Unlike <see cref="XcDumpHarness"/>, which slices and corrupts text,
/// this mutates at <em>token</em> granularity: a name for another name or a
/// type, a literal for one at a type's edge, an operator for another, a cast
/// or unary operator added. The result is usually still a parseable program,
/// which is what puts pressure on the type checker and the code generator
/// rather than on the lexer.</para>
/// </summary>
public static class ProgramDiffHarness
{
    private static readonly Regex Tok = new(
        @"//[^\n]*|/\*.*?\*/|\s+|""(?:\\.|[^""\\\n])*""|'(?:\\.|[^'\\\n])*'|" +
        @"0[xX][0-9A-Fa-f_]+\w*|\d[\d_]*\w*|[A-Za-z_]\w*|<<=|>>=|->|==|!=|<=|>=|&&|\|\||<<|>>|[-+*/%&|^]=|.",
        RegexOptions.Singleline | RegexOptions.Compiled);

    private static readonly Regex Ident = new(@"^[A-Za-z_]\w*$", RegexOptions.Compiled);
    private static readonly Regex Literal = new(@"^(\d\w*|0[xX]\w+|'.*')$", RegexOptions.Compiled);

    private static readonly string[] Types =
        { "u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64", "i128", "bool", "void" };

    private static readonly string[] Nums =
    {
        "0", "1", "2", "7", "31", "32", "63", "64", "127", "128", "255", "256", "65535",
        "0x7FFFFFFF", "0x80000000", "0xFFFFFFFF", "4294967296",
        "1u8", "300u8", "1u64", "128i8", "1i128", "'a'", "'\\n'", "true", "false",
    };

    private static readonly string[] Ops =
        { "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", ">", "<=", ">=", "&&", "||" };

    private static readonly string[] Casts =
        { " as u8", " as u32", " as i32", " as u64", " as u128", " as i8", " as *u8", " as bool", " as *phys u8" };

    private static readonly string[] Unary = { "&", "*", "-", "!", "~" };

    private sealed record Entry(string Path, string[] Includes, bool WithPrelude);

    /// <param name="prog">X program under test, e.g. "semadump".</param>
    /// <param name="verb">The matching `cxk` command, e.g. "sema".</param>
    /// <param name="counted">Output text whose occurrences are reported, as a sanity signal.</param>
    public static void Compare(string prog, string verb, string counted)
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();

        string? osDir = HostBuild.FindOsDir(repo, prog);
        Assert.SkipWhen(osDir is null,
            $"no CXOS checkout with os/xc/{prog}.xfxn was found. Set CXK_ROOT to the repository root.");

        using var work = new HostBuild.Workspace(prog);
        string exe = HostBuild.BuildDumpProgram(cxk, repo, osDir!, prog, work.Path);

        string prelude = Path.Combine(work.Path, "prelude.xfxn");
        var pr = HostBuild.Run(cxk, new[] { "prelude", prelude }, repo);
        Assert.True(pr.ExitCode == 0, $"`cxk prelude` failed: {pr.All}");

        string[] std = { "-I", Path.Combine(osDir!, "std") };
        string[] xc =
        {
            "-I", Path.Combine(osDir!, "xc", "host"),
            "-I", Path.Combine(osDir!, "xc"),
            "-I", Path.Combine(osDir!, "std"),
        };

        var corpus = new List<Entry>();
        foreach (string p in Sorted(Path.Combine(osDir!, "apps"), "*.xfxn")
                     .Concat(Sorted(Path.Combine(osDir!, "std"), "*.xfxn")))
            corpus.Add(new Entry(p, std, true));
        foreach (string p in Sorted(Path.Combine(osDir!, "xc"), "*.xfxn"))
            corpus.Add(new Entry(p, xc, false));
        foreach (string dir in SortedDirs(Path.Combine(repo, "devkit", "tests", "lang")))
            foreach (string p in Sorted(dir, "*.xfxn"))
                corpus.Add(new Entry(p, Array.Empty<string>(), false));

        Assert.SkipWhen(corpus.Count == 0, "no X programs found to compare.");

        var rng = new Random(TestEnv.Seed);
        var mutants = new List<Entry>();
        for (int k = 0; k < TestEnv.Mutants; k++)
        {
            Entry src = corpus[rng.Next(corpus.Count)];
            string dir = Path.Combine(work.Path, "m", $"{k:D5}");
            Directory.CreateDirectory(dir);
            string p = Path.Combine(dir, Path.GetFileName(src.Path));
            File.WriteAllText(p, Mutate(rng, File.ReadAllText(src.Path)));

            // The original's directory stays on the path, for what it imports
            // beside itself.
            string[] incs = src.Includes.Concat(new[] { "-I", Path.GetDirectoryName(src.Path)! }).ToArray();
            mutants.Add(new Entry(p, incs, src.WithPrelude));
        }

        long programs = 0, countedHits = 0, diagnostics = 0;

        foreach (var (label, items) in new[] { ("real sources", corpus), ("mutants", mutants) })
        {
            // Per label, not per run. Hoisting this outside the loop meant it
            // was already false by the time the mutants were reached, so the
            // sabotage never fired and the comparison could not be shown able
            // to fail - which is the whole point of having it.
            bool firstChunk = true;

            // Programs sharing their options go together.
            foreach (var group in items.GroupBy(e => (Key: string.Join('\u0001', e.Includes), e.WithPrelude)))
            {
                var files = group.Select(e => e.Path).ToList();
                string[] incs = group.First().Includes;
                bool pre = group.Key.WithPrelude;

                for (int k = 0; k < files.Count; k += 100)
                {
                    var chunk = files.Skip(k).Take(100).ToList();

                    var cArgs = new List<string> { verb };
                    if (!pre) cArgs.Add("--no-prelude");
                    cArgs.AddRange(incs);
                    cArgs.AddRange(chunk);

                    var xArgs = new List<string>();
                    if (pre) { xArgs.Add("--prelude"); xArgs.Add(prelude); } else xArgs.Add("--no-prelude");
                    xArgs.AddRange(incs);
                    xArgs.AddRange(chunk);

                    var c = HostBuild.Run(cxk, cArgs, repo);
                    var x = HostBuild.Run(exe, xArgs, work.Path);

                    Assert.True(x.ExitCode == 0, $"{prog} exited {x.ExitCode}: {x.StdErr}");

                    string csharp = c.StdOut;
                    string xside = x.StdOut;

                    if (TestEnv.Sabotage && label == "mutants" && firstChunk)
                        xside = xside.Length > 0 ? "sabotage\n" + xside : "sabotage\n";
                    firstChunk = false;

                    programs += Count(csharp, "\n== ") + (csharp.StartsWith("== ", StringComparison.Ordinal) ? 1 : 0);
                    countedHits += Count(csharp, counted);
                    diagnostics += Count(csharp, "\nerror ");

                    if (csharp != xside)
                        Assert.Fail(XcDumpHarness.Describe(label, csharp, xside));
                }
            }
        }

        // Not an assertion, a sanity signal: a run where nothing was counted
        // and nothing diagnosed compared very little, however green it looks.
        Assert.True(programs > 0, "the comparison saw no programs at all, so it compared nothing.");
        Console.WriteLine($"{countedHits} with '{counted.Trim()}' of {programs} programs; {diagnostics} diagnostics");
    }

    private static IEnumerable<string> Sorted(string dir, string pattern) =>
        Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, pattern).OrderBy(p => p, StringComparer.Ordinal)
            : Enumerable.Empty<string>();

    private static IEnumerable<string> SortedDirs(string dir) =>
        Directory.Exists(dir)
            ? Directory.EnumerateDirectories(dir).OrderBy(p => p, StringComparer.Ordinal)
            : Enumerable.Empty<string>();

    private static int Count(string haystack, string needle)
    {
        if (needle.Length == 0) return 0;
        int n = 0, i = 0;
        while ((i = haystack.IndexOf(needle, i, StringComparison.Ordinal)) >= 0) { n++; i += needle.Length; }
        return n;
    }

    private static string Mutate(Random rng, string text)
    {
        var toks = Tok.Matches(text).Select(m => m.Value).ToList();
        var code = new List<int>();
        for (int i = 0; i < toks.Count; i++)
        {
            string t = toks[i];
            if (t.Length == 0) continue;
            if (string.IsNullOrWhiteSpace(t)) continue;
            if (t.StartsWith("//", StringComparison.Ordinal) || t.StartsWith("/*", StringComparison.Ordinal)) continue;
            code.Add(i);
        }
        if (code.Count == 0) return text;

        var idents = code.Select(i => toks[i]).Where(t => Ident.IsMatch(t)).ToList();

        int rounds = rng.Next(1, 4);
        for (int r = 0; r < rounds; r++)
        {
            int i = code[rng.Next(code.Count)];
            string t = toks[i];
            double op = rng.NextDouble();

            if (Ident.IsMatch(t))
            {
                if (op < 0.55 && idents.Count > 0) toks[i] = idents[rng.Next(idents.Count)];
                else if (op < 0.70) toks[i] = Types[rng.Next(Types.Length)];
                else if (op < 0.85) toks[i] = t + Casts[rng.Next(Casts.Length)];
                else toks[i] = Unary[rng.Next(Unary.Length)] + t;
            }
            else if (Literal.IsMatch(t)) toks[i] = Nums[rng.Next(Nums.Length)];
            else if (Ops.Contains(t)) toks[i] = Ops[rng.Next(Ops.Length)];
            else if (t == ")" && op < 0.3) toks[i] = ")" + Casts[rng.Next(Casts.Length)];
            else if (op < 0.1) toks[i] = "";
        }

        string outText = string.Concat(toks);

        if (rng.NextDouble() < 0.15)
        {
            // A line doubled or dropped: declarations twice, or missing.
            var ls = outText.Split('\n').ToList();
            if (ls.Count > 0)
            {
                int j = rng.Next(ls.Count);
                if (rng.NextDouble() < 0.5) ls.Insert(j, ls[j]);
                else ls.RemoveAt(j);
                outText = string.Join("\n", ls);
            }
        }
        return outText;
    }
}
