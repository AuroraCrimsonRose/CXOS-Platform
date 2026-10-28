// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The type checker and code generator written in X, against the C# ones, on
/// whole programs, each built to run as x86-32 and as x86-64 (roadmap step
/// 6d1b). Ports <c>devkit/tests/xc/semadiff.py</c> and
/// <c>asmdiff.py</c> (HARDENING_PLAN D1).
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class SemaAsmDiffTests
{
    [Theory]
    [InlineData("x86_32")]
    [InlineData("x86_64")]
    public void Type_checker_in_X_agrees_with_the_C_sharp_checker(string host) =>
        ProgramDiffHarness.Compare(host, "semadump", "sema", "checked yes");

    [Theory]
    [InlineData("x86_32")]
    [InlineData("x86_64")]
    public void Code_generator_in_X_agrees_with_the_C_sharp_generator(string host) =>
        ProgramDiffHarness.Compare(host, "asmdump", "asm", "\n.text\n");

    [Theory]
    [InlineData("x86_32")]
    [InlineData("x86_64")]
    public void X86_64_back_end_in_X_agrees_with_the_C_sharp_back_end(string host) =>
        ProgramDiffHarness.Compare(host, "asmdump", "asm", "\n.text\n", "--target", "x86_64");

    [Theory]
    [InlineData("x86_32")]
    [InlineData("x86_64")]
    public void IR_lowering_in_X_agrees_with_the_C_sharp_lowering_on_x86_32(string host) =>
        ProgramDiffHarness.Compare(host, "irdump", "ir", "\nfn @", "--target", "x86_32");

    [Theory]
    [InlineData("x86_32")]
    [InlineData("x86_64")]
    public void IR_lowering_in_X_agrees_with_the_C_sharp_lowering_on_x86_64(string host) =>
        ProgramDiffHarness.Compare(host, "irdump", "ir", "\nfn @", "--target", "x86_64");
}
