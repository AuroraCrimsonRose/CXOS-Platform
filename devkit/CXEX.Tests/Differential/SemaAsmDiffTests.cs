// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The type checker, IR lowering and code generator written in X, against the
/// C# ones, on whole programs, each built to run as x86-64. Ports
/// <c>devkit/tests/xc/semadiff.py</c> and <c>asmdiff.py</c> (HARDENING_PLAN D1).
/// Until roadmap step 6d3c each also ran as x86-32, and the x86-32 emitters
/// were compared too; the IR still lowers for x86_32, so that comparison stays.
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class SemaAsmDiffTests
{
    [Fact]
    public void Type_checker_in_X_agrees_with_the_C_sharp_checker() =>
        ProgramDiffHarness.Compare("semadump", "sema", "checked yes");

    [Fact]
    public void X86_64_back_end_in_X_agrees_with_the_C_sharp_back_end() =>
        ProgramDiffHarness.Compare("asmdump", "asm", "\n.text\n");

    [Fact]
    public void IR_lowering_in_X_agrees_with_the_C_sharp_lowering_on_x86_32() =>
        ProgramDiffHarness.Compare("irdump", "ir", "\nfn @", "--target", "x86_32");

    [Fact]
    public void IR_lowering_in_X_agrees_with_the_C_sharp_lowering_on_x86_64() =>
        ProgramDiffHarness.Compare("irdump", "ir", "\nfn @", "--target", "x86_64");
}
