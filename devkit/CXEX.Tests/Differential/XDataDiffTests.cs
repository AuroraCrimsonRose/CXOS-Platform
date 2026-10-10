// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The DevKit's X Data reader against the OS's, document by document. Port of
/// <c>devkit/tests/xdata/difftest.py</c> (HARDENING_PLAN D1).
///
/// <para>Two readers of one format are worth having only if they agree, so
/// they are held to it: thousands of generated documents - valid, mutated, and
/// nested past the depth limit - go through both, and both the error code
/// <em>and the byte offset</em> must match. The offset matters as much as the
/// code: two readers can agree that a document is bad and disagree about
/// where, which means they disagree about the grammar.</para>
///
/// <para>The X side runs natively rather than under CXK: each batch of 200
/// documents is emitted as one X program that returns the 1-based index of the
/// first document it disagrees on, or 0.</para>
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class XDataDiffTests
{
    private static readonly string[] Ids =
        { "exec", "args", "start", "every", "grants", "a", "b_2", "x-y", "_k", "Service", "boot", "true", "false" };

    private static readonly string[] Alpha =
        "{}[]\",=/*\\\n\t -_0x9aZ".Select(c => c.ToString())
            .Concat(new[] { "//", "/*", "*/", "\\q", "\n\n" })
            .ToArray();

    [Fact]
    public void Both_X_Data_readers_agree_on_code_and_offset()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        Requires.Linux();

        string? std = new[] { Path.Combine(repo, "os", "std"), Path.Combine(repo, "CXK", "os", "std") }
            .FirstOrDefault(p => File.Exists(Path.Combine(p, "xdata.xfxn")));
        Assert.SkipWhen(std is null, "no CXOS checkout with os/std/xdata.xfxn (set CXK_ROOT).");

        using var work = new HostBuild.Workspace("xdiff");
        File.WriteAllText(Path.Combine(work.Path, "stub.s"), HostBuild.PlainStub);

        int n = TestEnv.Mutants;
        var rng = new Random(TestEnv.Seed);
        var docs = new List<string>(n);
        for (int i = 0; i < n; i++)
        {
            double r = rng.NextDouble();
            string doc = r < 0.02 ? Deep(rng.Next(28, 37), rng.NextDouble() < 0.5)
                       : r < 0.30 ? Entries(rng, 0)
                       : Mutate(rng, Entries(rng, 0));
            docs.Add(doc.Replace("\0", ""));
        }

        // ---- the C# side, in batches of 500 ----
        string docDir = Path.Combine(work.Path, "docs");
        Directory.CreateDirectory(docDir);
        var paths = new List<string>(n);
        for (int i = 0; i < n; i++)
        {
            string p = Path.Combine(docDir, $"{i:D5}.xd");
            File.WriteAllBytes(p, Encoding.ASCII.GetBytes(docs[i]));
            paths.Add(p);
        }

        var expected = new Dictionary<string, (int Code, int At)>(StringComparer.Ordinal);
        for (int c = 0; c < paths.Count; c += 500)
        {
            var chunk = paths.Skip(c).Take(500);
            var r = HostBuild.Run(cxk, new[] { "check-xdata", "--porcelain" }.Concat(chunk), repo);
            foreach (string line in r.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var parts = line.TrimEnd('\r').Split('\t');
                if (parts.Length == 3) expected[parts[0]] = (int.Parse(parts[1]), int.Parse(parts[2]));
            }
        }

        var exp = new List<(int Code, int At)>(n);
        foreach (string p in paths)
        {
            Assert.True(expected.ContainsKey(p), $"`cxk check-xdata` said nothing about {p}");
            exp.Add(expected[p]);
        }

        if (TestEnv.Sabotage)
        {
            // Skew one expected offset, to prove the comparison can still fail.
            int k = exp.FindIndex(e => e.Code != 0);
            if (k >= 0) exp[k] = (exp[k].Code, exp[k].At + 1);
        }

        // ---- the X side, 200 documents per generated program ----
        var failures = new List<string>();
        for (int b = 0; b < n; b += 200)
        {
            var idx = Enumerable.Range(b, Math.Min(200, n - b)).ToList();

            var src = new StringBuilder();
            src.AppendLine("import \"xdata.xfxn\";");
            src.AppendLine("fn tlen(s: *u8) -> u32 { let n: u32 = 0; while (s[n] != 0) { n = n + 1; } return n; }");
            src.AppendLine("fn t(d: *u8, code: i32, at: u32) -> bool { " +
                           "if (xd_check(d, tlen(d)) != code) { return false; } " +
                           "if (code == 0) { return true; } return xd_err_at == at; }");
            src.AppendLine("fn main() -> i32 {");
            for (int j = 0; j < idx.Count; j++)
            {
                var (code, at) = exp[idx[j]];
                // X has no negative literals here, so a negative code is written
                // as a subtraction, exactly as the Python harness did.
                string codeText = code >= 0 ? code.ToString() : $"0 - {-code}";
                src.AppendLine($"    if (!t({XLiteral(docs[idx[j]])}, {codeText}, {at})) {{ return {j + 1}; }}");
            }
            src.AppendLine("    return 0;");
            src.AppendLine("}");

            string xf = Path.Combine(work.Path, $"batch{b / 200}.xfxn");
            File.WriteAllText(xf, src.ToString());
            string baseName = xf[..^5];
            if (File.Exists(baseName + ".s")) File.Delete(baseName + ".s");

            var cr = HostBuild.Run(cxk,
                new[] { "compile", xf, baseName + ".elf", "--no-prelude", "--emit-asm", "-I", std! }, repo);

            // `cxk compile` exits nonzero when it cannot assemble, which is not
            // this test's business: only the emitted .s matters, and it is
            // linked natively below.
            Assert.True(cr.StdOut.Contains("emitted", StringComparison.Ordinal),
                $"X compile failed:\n{cr.All}");

            HostBuild.Link(baseName + ".s", baseName + ".bin", HostBuild.PlainStub, work.Path);
            var x = HostBuild.Run(baseName + ".bin", Array.Empty<string>(), work.Path, 300_000);

            if (x.ExitCode != 0)
            {
                int i = idx[x.ExitCode - 1];
                failures.Add($"batch {b / 200} doc {i}: C# says (code {exp[i].Code}, at {exp[i].At}); " +
                             $"doc={Printable(docs[i])}");
            }
        }

        Assert.True(failures.Count == 0,
            $"the two X Data readers disagree on {failures.Count} batch(es):\n  " +
            string.Join("\n  ", failures));
    }

    private static string Deep(int n, bool brackets) =>
        "k = " + (brackets
            ? new string('[', n) + new string(']', n)
            : string.Concat(Enumerable.Repeat("{ k = ", n)) + "1" + string.Concat(Enumerable.Repeat(" }", n)));

    private static string Ws(Random rng) =>
        new[] { "", " ", "  ", "\t", " // c\n", " /* c */ ", "\n", "\r\n" }[rng.Next(8)];

    private static string Ident(Random rng) => Ids[rng.Next(Ids.Length)];

    private static string Str(Random rng)
    {
        string[] bits = { "a", " ", "\\n", "\\\"", "\\\\", "\\t", "\\0", "z", "/", "{", "}", "[", "#" };
        int len = rng.Next(0, 7);
        var sb = new StringBuilder("\"");
        for (int i = 0; i < len; i++) sb.Append(bits[rng.Next(bits.Length)]);
        return sb.Append('"').ToString();
    }

    private static string Integer(Random rng) => new[]
    {
        "0", "7", "-12", "300", "0x1F", "0x0100_0000", "1_000", "4294967295", "99999999999", "-0",
    }[rng.Next(10)];

    private static string Value(Random rng, int depth)
    {
        double r = rng.NextDouble();
        if (depth > 4 || r < 0.25)
            return rng.Next(3) switch { 0 => Str(rng), 1 => Integer(rng), _ => Ident(rng) };

        if (r < 0.5)
        {
            var seps = Enumerable.Range(0, 4)
                .Select(_ => new[] { ", ", ",\n", "\n", "," }[rng.Next(4)]).ToArray();
            int count = rng.Next(0, 4);
            var items = Enumerable.Range(0, count).Select(_ => Value(rng, depth + 1)).ToList();
            var sb = new StringBuilder("[");
            for (int i = 0; i < items.Count; i++)
            {
                sb.Append(Ws(rng)).Append(items[i]);
                sb.Append(i < items.Count - 1 ? seps[i] : new[] { "", "," }[rng.Next(2)]);
            }
            return sb.Append(Ws(rng)).Append(']').ToString();
        }

        string tag = rng.NextDouble() < 0.3 ? Ident(rng) + " " : "";
        return tag + "{" + Entries(rng, depth + 1) + "}";
    }

    private static string Entries(Random rng, int depth)
    {
        // rng.sample(IDS, k): k distinct keys, in random order.
        var pool = Ids.ToList();
        int k = rng.Next(0, 5);
        var keys = new List<string>();
        for (int i = 0; i < k && pool.Count > 0; i++)
        {
            int j = rng.Next(pool.Count);
            keys.Add(pool[j]);
            pool.RemoveAt(j);
        }

        var sb = new StringBuilder();
        for (int i = 0; i < keys.Count; i++)
        {
            sb.Append(Ws(rng)).Append(keys[i])
              .Append(new[] { " = ", "=", " =\n" }[rng.Next(3)])
              .Append(Value(rng, depth));
            if (i < keys.Count - 1) sb.Append(new[] { ",", "\n", ", ", ",\n" }[rng.Next(4)]);
        }
        return sb.Append(Ws(rng)).ToString();
    }

    private static string Mutate(Random rng, string s)
    {
        int rounds = rng.Next(1, 4);
        for (int i = 0; i < rounds; i++)
        {
            int p = rng.Next(0, s.Length + 1);
            double op = rng.NextDouble();
            string frag = Alpha[rng.Next(Alpha.Length)];
            if (op < 0.4) s = s[..p] + frag + s[p..];
            else if (op < 0.7 && s.Length > 0 && p < s.Length) s = s[..p] + s[(p + 1)..];
            else if (s.Length > 0 && p < s.Length) s = s[..p] + frag + s[(p + 1)..];
        }
        return s;
    }

    /// <summary>The document as an X string literal.</summary>
    private static string XLiteral(string s) =>
        "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"")
                .Replace("\n", "\\n").Replace("\t", "\\t").Replace("\r", "\\r") + "\"";

    private static string Printable(string s) =>
        s.Length <= 200 ? XLiteral(s) : XLiteral(s[..200]) + $" (+{s.Length - 200} more)";
}
