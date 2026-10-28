// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.IR;
using CXEX.Lang.Lexer;
using CXEX.Lang.Parsing;
using CXEX.Lang.Sema;
using CXEX.Tests.Differential;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The x86-64 target (roadmap step 6b3) where no Linux host is needed: the
/// layout the front end gives each target, the IR at 8-byte pointers pinned
/// by a golden file, and every X program in the tree through the back end and
/// clang's assembler. Running the programs is <see cref="X64RunTests"/>.
/// </summary>
[Trait(Categories.Key, Categories.Unit)]
public class X64BackEndTests
{
    private static (SemaContext Ctx, IrModule Ir) Lower(string source, XTarget target)
    {
        var diag = new DiagnosticBag();
        var unit = new Parser(new Lexer(source, "t.xfxn", diag).Tokenize(), diag).Parse();
        var ctx = new Resolver(diag, target).Resolve(unit);
        var tc = new TypeChecker(ctx, diag);
        if (!diag.HasErrors) tc.Check(unit);
        Assert.False(diag.HasErrors, string.Join('\n', diag.Items.Select(d => $"{d.Span.Line}:{d.Span.Col} {d.Message}")));
        return (ctx, new IrLowering(ctx, tc.LocalTypes, diag).Lower(unit));
    }

    private const string Shapes =
        "struct s { a: u32, b: u64, c: u8 }\n" +
        "struct p { a: u32, q: *u8 }\n" +
        "struct w { a: u32, v: u128 }\n" +
        "enum e { none, one { x: u64 }, two { y: u32 } }\n" +
        "fn main() -> i32 { return 0; }\n";

    /* On x86-32 every field is on 4, as X has always been; on x86-64 a u64
       or a pointer is on 8 and an i128 on 16, as System V lays out C, and a
       sum type's payload starts on its own alignment. */
    [Theory]
    [InlineData("x86_32", 4, 16, 4, 8, 4, 20, 4, 12)]
    [InlineData("x86_64", 8, 24, 8, 16, 16, 32, 8, 16)]
    public void Layout_follows_the_target(string name, int bAt, int sSize, int qAt, int pSize, int vAt, int wSize, int payloadAt, int eSize)
    {
        var (ctx, _) = Lower(Shapes, XTarget.Parse(name)!);
        var s = ctx.Structs["s"];
        Assert.Equal(bAt, ctx.FieldOffset(s, "b"));
        Assert.Equal(bAt + 8, ctx.FieldOffset(s, "c"));
        Assert.Equal(sSize, ctx.StructSize(s.Fields));
        Assert.Equal(qAt, ctx.FieldOffset(ctx.Structs["p"], "q"));
        Assert.Equal(pSize, ctx.StructSize(ctx.Structs["p"].Fields));
        Assert.Equal(vAt, ctx.FieldOffset(ctx.Structs["w"], "v"));
        Assert.Equal(wSize, ctx.StructSize(ctx.Structs["w"].Fields));
        Assert.Equal(payloadAt, ctx.PayloadOffset(ctx.Enums["e"]));
        Assert.Equal(payloadAt, ctx.FieldOffset(ctx.Structs["e"], "$body"));
        Assert.Equal(eSize, ctx.SizeOf(new NamedType("e")));
    }

    [Fact]
    public void A_usize_is_as_wide_as_the_target_pointer()
    {
        const string src = "global g: usize = 0xFFFF_FFFF_FFFFusize;\nfn main() -> i32 { return 0; }\n";
        var (_, ir) = Lower(src, XTarget.X86_64);
        Assert.Equal(8, ir.PointerBytes);
        Assert.Equal(8, ir.Globals.Single(x => x.Name == "g").Size);

        var diag = new DiagnosticBag();
        var unit = new Parser(new Lexer(src, "t.xfxn", diag).Tokenize(), diag).Parse();
        var ctx = new Resolver(diag, XTarget.X86_32).Resolve(unit);
        new TypeChecker(ctx, diag).Check(unit);
        Assert.Contains(diag.Items, d => d.Message.Contains("does not fit in usize", StringComparison.Ordinal));
    }

    /* The golden programs lowered at 8-byte pointers: the text the back end
       reads, pinned. Regenerate with `cxk ir --no-prelude --target x86_64`
       minus its `==` line, and review it by hand. */
    [Fact]
    public void The_x86_64_IR_text_matches_the_golden_file()
    {
        string dir = Path.Combine(Requires.Repo(), "devkit", "tests", "ir", "golden");
        var progs = Directory.GetFiles(dir, "*.xfxn").Where(p => File.Exists(Path.ChangeExtension(p, ".x86_64.ir"))).ToList();
        Assert.NotEmpty(progs);
        foreach (var p in progs)
        {
            var (_, ir) = Lower(File.ReadAllText(p), XTarget.X86_64);
            Assert.Empty(IrVerifier.Verify(ir));
            string want = File.ReadAllText(Path.ChangeExtension(p, ".x86_64.ir")).Replace("\r\n", "\n");
            Assert.Equal(want, IrPrinter.Print(ir));
        }
    }

    /* Every X program in the tree, as IrTests lists them, compiled for x86-64
       to an object: lowered, verified, emitted and assembled by clang for
       x86_64-elf. Nothing runs here; a back end that emits what the
       assembler refuses, or throws on an instruction, fails. */
    [Fact]
    public void Every_X_program_in_the_tree_compiles_for_x86_64()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        Requires.Toolchain();
        string os = Path.Combine(repo, "os");
        string tests = Path.Combine(repo, "devkit", "tests");
        string[] std = { "-I", Path.Combine(os, "std") };
        string[] xc = CXEX.Tests.Differential.HostBuild.XcIncludes(os, "x86_64").Append("--no-prelude").ToArray();
        IEnumerable<string> In(string d) => Directory.Exists(d) ? Directory.GetFiles(d, "*.xfxn").OrderBy(p => p, StringComparer.Ordinal) : Array.Empty<string>();
        var programs = In(Path.Combine(os, "apps")).Concat(In(Path.Combine(os, "std"))).Select(p => (p, std))
            .Concat(In(Path.Combine(os, "xc")).Where(p => Path.GetFileNameWithoutExtension(p) is var n && (n == "xc" || n.EndsWith("dump", StringComparison.Ordinal))).Select(p => (p, xc)))
            .Concat(new[] { "run", "interop", "interop64" }.SelectMany(d => In(Path.Combine(tests, "lang", d)))
                .Concat(In(Path.Combine(tests, "ir", "run"))).Concat(In(Path.Combine(tests, "ir", "golden"))).Concat(In(Path.Combine(tests, "x64", "run")))
                .Select(p => (p, new[] { "--no-prelude" })))
            .ToList();
        Assert.True(programs.Count >= 40, $"only {programs.Count} programs found");

        using var work = new HostBuild.Workspace("x64obj");
        var failures = new List<string>();
        foreach (var (src, args) in programs)
        {
            string obj = Path.Combine(work.Path, Path.GetFileName(Path.GetDirectoryName(src)) + "_" + Path.GetFileNameWithoutExtension(src) + ".o");
            var r = HostBuild.Run(cxk, new[] { "compile", src, obj, "--object", "--target", "x86_64" }.Concat(args), repo);
            if (r.ExitCode != 0 || !File.Exists(obj))
                failures.Add($"{Path.GetRelativePath(repo, src)}: exit {r.ExitCode}\n{string.Join('\n', r.All.Split('\n').Where(l => l.Contains("error", StringComparison.OrdinalIgnoreCase)).Take(8))}");
        }
        Assert.True(failures.Count == 0, $"{failures.Count} of {programs.Count} did not compile for x86-64:\n" + string.Join("\n\n", failures));
    }

    /* The prelude `cxk` puts in front of a program compiled for x86-64 has
       that machine's wrappers (roadmap step 6d1a): no syscall wrapper cuts a
       pointer or a u64 to 32 bits, which the x86-32 ones do to fill a u32
       register or a pair. Checked through `cxk ir`, so the target has to reach
       the prelude from the command line. */
    [Fact]
    public void The_x86_64_prelude_passes_pointers_and_u64s_whole()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        string os = Path.Combine(repo, "os");
        var r = HostBuild.Run(cxk, new[] { "ir", Path.Combine(os, "apps", "hi.xfxn"), "--target", "x86_64", "-I", Path.Combine(os, "std") }, repo);
        Assert.True(r.ExitCode == 0, r.All);

        string[] wrappers = { "console_write", "file_read", "file_seek", "vm_map", "exec_path" };
        foreach (string w in wrappers)
        {
            int at = r.StdOut.IndexOf($"fn @{w}(", StringComparison.Ordinal);
            Assert.True(at >= 0, $"@{w} is not in the IR");
            int end = r.StdOut.IndexOf("\n}", at, StringComparison.Ordinal);
            string body = r.StdOut[at..end];
            Assert.True(!body.Contains("trunc i64", StringComparison.Ordinal), $"@{w} cuts a 64-bit argument:\n{body}");
        }
    }
}
