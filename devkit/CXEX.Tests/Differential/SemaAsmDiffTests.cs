// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The type checker and code generator written in X, against the C# ones, on
/// whole programs. Ports <c>devkit/tests/xc/semadiff.py</c> and
/// <c>asmdiff.py</c> (HARDENING_PLAN D1).
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class SemaAsmDiffTests
{
    [Fact]
    public void Type_checker_in_X_agrees_with_the_C_sharp_checker() =>
        ProgramDiffHarness.Compare("semadump", "sema", "checked yes");

    [Fact]
    public void Code_generator_in_X_agrees_with_the_C_sharp_generator() =>
        ProgramDiffHarness.Compare("asmdump", "asm", "\n.text\n");
}
