// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// Compares a dump program written in X (<c>os/xc</c>) against the `cxk`
/// command printing the same format, over every real source in the tree and
/// over mutants of windows of them.
///
/// <para>Port of <c>devkit/tests/xc/xcdiff.py</c> (HARDENING_PLAN D1). The
/// corpus, the mutation operators and the comparison are the same; the
/// generator is .NET's, so a given seed does not reproduce the Python run's
/// mutants - only its own, which is what reproducibility needs.</para>
///
/// <para>The C# compiler is the oracle while <c>os/xc</c> matches it
/// (CX_X_CORE_LANG §10), so any disagreement is a real defect on one side.</para>
/// </summary>
public static class XcDumpHarness
{
    /// <summary>
    /// UTF-8 that substitutes U+FFFD rather than throwing. Deleting one half of
    /// a surrogate pair is a mutation like any other, and .NET strings are
    /// UTF-16 so it is reachable; the default encoder throws on the result and
    /// kills the run instead of feeding the lexer the malformed input this
    /// exists to produce. (Python never hit this: its strings are code points.)
    /// </summary>
    private static readonly System.Text.Encoding Utf8Lenient =
        new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: false);


    /// <param name="prog">X program under test, e.g. "tokdump".</param>
    /// <param name="verb">`cxk` command printing the same format, e.g. "tokens".</param>
    /// <param name="alphabet">Fragments the mutator splices in.</param>
    /// <param name="window">Min/max slice of a real source to mutate.</param>
    /// <param name="edits">Upper bound on character edits per mutant.</param>
    /// <param name="lineOps">Also drop, double and swap whole lines.</param>
    public static void Compare(string prog, string verb, string[] alphabet,
                               (int Lo, int Hi) window, int edits, bool lineOps = false)
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
        var rng = new Random(TestEnv.Seed);

        // ---- the corpus: every real source, then mutants of windows of them ----
        var real = Directory.EnumerateFiles(osDir!, "*.xfxn", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(Path.Combine(repo, "devkit", "tests"), "*.xfxn",
                                             SearchOption.AllDirectories))
            .Distinct()
            .OrderBy(p => p, StringComparer.Ordinal)
            .ToList();

        Assert.SkipWhen(real.Count == 0, "no .xfxn sources found to compare.");

        var texts = real.Select(File.ReadAllText).ToList();

        string mutantDir = Path.Combine(work.Path, "m");
        Directory.CreateDirectory(mutantDir);
        var mutants = new List<string>();
        for (int i = 0; i < TestEnv.Mutants; i++)
        {
            string src = texts[rng.Next(texts.Count)];
            int start = rng.Next(0, Math.Max(1, src.Length - window.Hi));
            int len = Math.Min(rng.Next(window.Lo, window.Hi + 1), Math.Max(0, src.Length - start));
            string doc = Mutate(rng, src.Substring(start, len), alphabet, edits, lineOps);

            string p = Path.Combine(mutantDir, $"m{i:D5}.xfxn");
            File.WriteAllText(p, doc, Utf8Lenient);
            mutants.Add(p);
        }

        // ---- compare, 300 files at a time ----
        foreach (var (label, files) in new[] { ("real sources", real), ("mutants", mutants) })
        {
            for (int k = 0; k < files.Count; k += 300)
            {
                var chunk = files.Skip(k).Take(300).ToList();

                var c = HostBuild.Run(cxk, new[] { verb }.Concat(chunk), repo, 600_000);
                var x = HostBuild.Run(exe, chunk, work.Path, 600_000);

                Assert.True(x.ExitCode == 0, $"{prog} exited {x.ExitCode}: {x.StdErr}");

                string csharp = c.StdOut;
                string xside = x.StdOut;

                // Prove the comparison can still see a difference.
                if (TestEnv.Sabotage && label == "mutants" && k == 0)
                    xside = ReplaceFirst(xside, "\n", "\nsabotage\n");

                if (csharp != xside)
                    Assert.Fail(Describe(label, csharp, xside));
            }
        }
    }

    /// <summary>
    /// Names the first differing output line and the source it belongs to.
    /// The dumps are megabytes; a raw equality failure is unreadable.
    /// </summary>
    internal static string Describe(string label, string csharp, string xside)
    {
        var cl = csharp.Split('\n');
        var xl = xside.Split('\n');
        int i = 0;
        while (i < cl.Length && i < xl.Length && cl[i] == xl[i]) i++;

        string header = "?";
        for (int j = Math.Min(i, cl.Length - 1); j >= 0; j--)
        {
            if (cl[j].StartsWith("== ", StringComparison.Ordinal)) { header = cl[j][3..]; break; }
        }

        return $"DIFFER ({label}) in {header} at output line {i}:\n" +
               $"  C#: {(i < cl.Length ? cl[i] : "<end>")}\n" +
               $"  X:  {(i < xl.Length ? xl[i] : "<end>")}";
    }

    private static string ReplaceFirst(string s, string find, string replace)
    {
        int i = s.IndexOf(find, StringComparison.Ordinal);
        return i < 0 ? s : string.Concat(s.AsSpan(0, i), replace, s.AsSpan(i + find.Length));
    }

    private static string Mutate(Random rng, string text, string[] alphabet, int edits, bool lineOps)
    {
        if (lineOps && rng.NextDouble() < 0.3)
        {
            // Whole lines dropped, doubled or moved - what breaks nesting.
            var ls = text.Split('\n').ToList();
            int rounds = rng.Next(1, 4);
            for (int r = 0; r < rounds && ls.Count > 0; r++)
            {
                int i = rng.Next(ls.Count), j = rng.Next(ls.Count);
                double op = rng.NextDouble();
                if (op < 0.4) ls.RemoveAt(i);
                else if (op < 0.7) ls.Insert(j, ls[i]);
                else (ls[i], ls[j]) = (ls[j], ls[i]);
            }
            text = string.Join("\n", ls);
        }

        var buf = new List<char>(text);
        int count = rng.Next(1, edits + 1);
        for (int e = 0; e < count; e++)
        {
            int p = rng.Next(0, buf.Count + 1);
            double op = rng.NextDouble();
            if (op < 0.45)
            {
                buf.InsertRange(Math.Min(p, buf.Count), alphabet[rng.Next(alphabet.Length)]);
            }
            else if (op < 0.75 && buf.Count > 0)
            {
                buf.RemoveAt(Math.Min(p, buf.Count - 1));
            }
            else if (buf.Count > 0)
            {
                // One character out, the whole fragment in. Taking only
                // frag[0] would split "😀" and leave a lone surrogate, which
                // is not encodable as UTF-8 - the file write then throws
                // rather than producing the malformed input this is meant to
                // be feeding the lexer.
                int at = Math.Min(p, buf.Count - 1);
                buf.RemoveAt(at);
                buf.InsertRange(at, alphabet[rng.Next(alphabet.Length)]);
            }
        }
        return new string(buf.ToArray());
    }
}
