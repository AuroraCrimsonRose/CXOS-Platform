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
/// The typed IR (roadmap step 6b2, <c>devkit/CXEX.Lang/IR/IR.md</c>): every X
/// program in the tree lowers to IR the verifier accepts; the run programs,
/// lowered and run by <see cref="IrInterpreter"/>, still return 0; the text is
/// pinned by a golden file; and the verifier refuses each kind of malformed
/// module it exists to catch.
/// </summary>
[Trait(Categories.Key, Categories.Unit)]
public class IrTests
{
    /* The front end without imports or the prelude: what the run programs
       and the IR tests need. `cxk ir` covers the programs that import. */
    private static IrModule Lower(string path)
    {
        var diag = new DiagnosticBag();
        var toks = new Lexer(File.ReadAllText(path), Path.GetFileName(path), diag).Tokenize();
        var unit = new Parser(toks, diag).Parse();
        var ctx = new Resolver(diag, XTarget.X86_32).Resolve(unit);
        TypeChecker? tc = null;
        if (!diag.HasErrors) { tc = new TypeChecker(ctx, diag); tc.Check(unit); }
        IrModule? ir = null;
        if (!diag.HasErrors) ir = new IrLowering(ctx, tc!.LocalTypes, diag).Lower(unit);
        Assert.False(diag.HasErrors, $"{path}:\n" + string.Join('\n', diag.Items.Select(d => $"{d.Span.Line}:{d.Span.Col} {d.Message}")));
        return ir!;
    }

    private static IEnumerable<string> Programs(string dir) =>
        Directory.GetFiles(dir, "*.xfxn").OrderBy(p => p, StringComparer.Ordinal);

    [Fact]
    public void Run_programs_return_zero_when_their_IR_is_interpreted()
    {
        string tests = Path.Combine(Requires.Repo(), "devkit", "tests");
        var progs = Programs(Path.Combine(tests, "lang", "run")).Concat(Programs(Path.Combine(tests, "ir", "run"))).ToList();
        Assert.True(progs.Count >= 15, $"only {progs.Count} programs found");
        var failures = new List<string>();
        foreach (var p in progs)
        {
            var ir = Lower(p);
            var problems = IrVerifier.Verify(ir);
            if (problems.Count > 0) { failures.Add($"{Path.GetFileName(p)}: verifier: {string.Join("; ", problems.Take(5))}"); continue; }
            try
            {
                var r = new IrInterpreter(ir).Call("main");
                if (r != 0) failures.Add($"{Path.GetFileName(p)}: main returned {r}");
            }
            catch (IrInterpreter.Trap t) { failures.Add($"{Path.GetFileName(p)}: trapped: {t.Message}"); }
        }
        Assert.True(failures.Count == 0, string.Join('\n', failures));
    }

    [Fact]
    public void The_IR_text_matches_the_golden_file()
    {
        string dir = Path.Combine(Requires.Repo(), "devkit", "tests", "ir", "golden");
        var progs = Programs(dir).ToList();
        Assert.NotEmpty(progs);
        foreach (var p in progs)
        {
            var ir = Lower(p);
            Assert.Empty(IrVerifier.Verify(ir));
            string want = File.ReadAllText(Path.ChangeExtension(p, ".ir")).Replace("\r\n", "\n");
            Assert.Equal(want, IrPrinter.Print(ir));
        }
    }

    /* Every X program in the tree that compiles on its own, with the options
       the differential tests use, through `cxk ir`: no diagnostic, no
       verifier complaint. Of os/xc that is the programs (xc and the dump
       tools); the rest are units they import. tests/lang/std is left out: its
       programs link against stubs LangRunTests supplies. */
    [Fact]
    public void Every_X_program_in_the_tree_lowers_to_IR_the_verifier_accepts()
    {
        string repo = Requires.Repo();
        string cxk = Requires.Cxk();
        string os = Path.Combine(repo, "os");
        string tests = Path.Combine(repo, "devkit", "tests");
        string[] std = { "-I", Path.Combine(os, "std") };
        string[] xc = CXEX.Tests.Differential.HostBuild.XcIncludes(os);
        var groups = new (IEnumerable<string> Files, string[] Args)[]
        {
            (Programs(Path.Combine(os, "apps")).Concat(Programs(Path.Combine(os, "std"))), std),
            (Programs(Path.Combine(os, "xc")).Where(p => Path.GetFileNameWithoutExtension(p) is var n && (n == "xc" || n.EndsWith("dump", StringComparison.Ordinal))),
             xc.Append("--no-prelude").ToArray()),
            (new[] { "run", "interop" }.SelectMany(d => Programs(Path.Combine(tests, "lang", d)))
                .Concat(Programs(Path.Combine(tests, "ir", "run"))).Concat(Programs(Path.Combine(tests, "ir", "golden"))),
             new[] { "--no-prelude" }),
        };
        int programs = 0;
        var failures = new List<string>();
        foreach (var (files, args) in groups)
        {
            var list = files.ToList();
            programs += list.Count;
            var r = HostBuild.Run(cxk, new[] { "ir" }.Concat(args).Concat(list), repo);
            var errs = r.StdOut.Split('\n').Where(l => l.StartsWith("error", StringComparison.Ordinal)).ToList();
            int dumped = r.StdOut.Split('\n').Count(l => l.StartsWith("module pointer ", StringComparison.Ordinal));
            if (r.ExitCode != 0 || errs.Count > 0 || dumped != list.Count)
                failures.Add($"exit {r.ExitCode}, {dumped}/{list.Count} dumped:\n{string.Join('\n', errs.Take(20))}\n{r.StdErr}");
        }
        Assert.True(programs >= 40, $"only {programs} programs found");
        Assert.True(failures.Count == 0, string.Join("\n\n", failures));
    }

    // ---- the verifier refuses what it exists to catch ----

    private static IrModule One(Action<IrFunction> build, IrParam? ret = null)
    {
        var m = new IrModule { PointerBytes = 4 };
        var f = new IrFunction { Name = "f", Return = ret ?? new IrParam("", IrType.I32) };
        build(f);
        m.Functions.Add(f);
        return m;
    }
    private static IrBlock Block(IrFunction f, IrTerm? t, params IrInstr[] ins)
    {
        var b = new IrBlock { Id = f.Blocks.Count, Term = t };
        b.Instrs.AddRange(ins);
        f.Blocks.Add(b);
        return b;
    }
    private static readonly IrReg R0 = new(0, IrType.I32), R1 = new(1, IrType.I32);
    private static IrConst I(int v) => new(IrType.I32, (UInt128)v);

    private static void Refused(IrModule m, string expected)
    {
        var problems = IrVerifier.Verify(m);
        Assert.True(problems.Exists(p => p.Contains(expected, StringComparison.Ordinal)),
            $"expected \"{expected}\", got: {string.Join("; ", problems)}");
    }

    [Fact]
    public void A_well_formed_module_passes() =>
        Assert.Empty(IrVerifier.Verify(One(f =>
        {
            Block(f, new IrBranch(new IrConst(IrType.Bool, 1), 1, 2), new IrBinary(R0, IrBinOp.Add, I(1), I(2)));
            Block(f, new IrRet(R0));
            Block(f, new IrRet(I(0)));
        })));

    private static readonly IrConst P0 = new(IrType.Ptr, 0);

    [Fact]
    public void A_well_formed_atomic_passes() =>
        Assert.Empty(IrVerifier.Verify(One(f => Block(f, new IrRet(R0),
            new IrAtomic(null, IrAtomicOp.Store, IrType.I32, P0, I(1)),
            new IrAtomic(R0, IrAtomicOp.Cas, IrType.I32, P0, I(1), I(2))))));

    [Fact]
    public void Verifier_refuses_an_atomic_on_a_narrow_type() =>
        Refused(One(f => Block(f, new IrRet(I(0)), new IrAtomic(new IrReg(0, IrType.I8), IrAtomicOp.Load, IrType.I8, P0))),
            "atomic on i8");

    [Fact]
    public void Verifier_refuses_a_cas_without_its_new_value() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrAtomic(R0, IrAtomicOp.Cas, IrType.I32, P0, I(1)))),
            "atomic Cas with 1 operands");

    [Fact]
    public void Verifier_refuses_an_atomic_store_with_a_result() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrAtomic(R0, IrAtomicOp.Store, IrType.I32, P0, I(1)))),
            "atomic Store result");

    [Fact]
    public void Verifier_refuses_an_atomic_operand_of_another_type() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrAtomic(R0, IrAtomicOp.Add, IrType.I32, P0, new IrConst(IrType.I64, 1)))),
            "atomic operand");

    [Fact]
    public void Verifier_refuses_a_block_without_a_terminator() =>
        Refused(One(f => Block(f, null)), "no terminator");

    [Fact]
    public void Verifier_refuses_a_register_assigned_twice() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrBinary(R0, IrBinOp.Add, I(1), I(2)), new IrBinary(R0, IrBinOp.Add, I(1), I(2)))),
            "%0 assigned twice");

    [Fact]
    public void Verifier_refuses_a_use_before_the_assignment() =>
        Refused(One(f => Block(f, new IrRet(R1), new IrBinary(R1, IrBinOp.Add, R0, I(1)), new IrBinary(R0, IrBinOp.Add, I(1), I(2)))),
            "%0 is used where its assignment does not always run first");

    /* Assigned on one arm of a branch, used where both arms meet. */
    [Fact]
    public void Verifier_refuses_a_use_its_assignment_does_not_dominate() =>
        Refused(One(f =>
        {
            Block(f, new IrBranch(new IrConst(IrType.Bool, 1), 1, 2));
            Block(f, new IrJump(2), new IrBinary(R0, IrBinOp.Add, I(1), I(2)));
            Block(f, new IrRet(R0));
        }), "%0 is used where its assignment does not always run first");

    [Fact]
    public void Verifier_refuses_mismatched_operand_types() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrBinary(R0, IrBinOp.Add, I(1), new IrConst(IrType.I64, 2)))),
            "Add operand is i64, wants i32");

    [Fact]
    public void Verifier_refuses_a_branch_on_an_integer() =>
        Refused(One(f => { Block(f, new IrBranch(I(1), 1, 1)); Block(f, new IrRet(I(0))); }),
            "branch condition is i32, wants bool");

    [Fact]
    public void Verifier_refuses_a_jump_to_a_missing_block() =>
        Refused(One(f => Block(f, new IrJump(7))), "jump to missing b7");

    [Fact]
    public void Verifier_refuses_a_return_of_the_wrong_type() =>
        Refused(One(f => Block(f, new IrRet(new IrConst(IrType.I8, 1)))), "ret value is i8, wants i32");

    [Fact]
    public void Verifier_refuses_a_widening_trunc() =>
        Refused(One(f => Block(f, new IrRet(I(0)), new IrConvert(new IrReg(0, IrType.I64), IrConv.Trunc, I(1)))),
            "Trunc from i32 to i64");

    [Fact]
    public void Verifier_refuses_a_constant_wider_than_its_type() =>
        Refused(One(f => Block(f, new IrRet(new IrConst(IrType.I32, (UInt128)1 << 32)))), "does not fit i32");

    [Fact]
    public void Verifier_refuses_an_unknown_symbol() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrLoad(R0, new IrSym("nowhere")))), "unknown symbol @nowhere");

    [Fact]
    public void Verifier_refuses_a_call_that_does_not_match_the_signature() =>
        Refused(One(f => Block(f, new IrRet(R0), new IrCall(R0, new IrSym("f"), new List<IrArg> { new(I(1)) }))),
            "@f takes 0 args, given 1");

    [Fact]
    public void Verifier_refuses_a_duplicate_switch_case() =>
        Refused(One(f =>
        {
            Block(f, new IrSwitch(I(1), 1, new List<(UInt128, int)> { (3, 1), (3, 1) }));
            Block(f, new IrRet(I(0)));
        }), "switch case 3 twice");

    [Fact]
    public void Verifier_refuses_a_global_initializer_of_the_wrong_size()
    {
        var m = new IrModule { PointerBytes = 4 };
        m.Globals.Add(new IrGlobal { Name = "g", Size = 8, Align = 4, Init = new List<IrData> { new IrDataBytes(new byte[4]) } });
        Refused(m, "initializer is 4 bytes, the global 8");
    }
}
