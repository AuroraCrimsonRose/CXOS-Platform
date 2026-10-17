// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// How <c>cxk verify-boot</c> reads a serial log. The lines are real ones from
/// boots on 2026-10-16. The rules came over from bootverify.py, which learned
/// them the hard way: a panic beats a summary, the self-tests' deliberate
/// refusals are not failures, and silence is never a pass.
/// </summary>
public class BootVerdictTests
{
    private const string Summary = "[KTEST   ] - [OK  ] - self-tests: all 45 passed\n";
    private const string Refusal = "-> signature check failed - unsigned, or signed by a key this kernel does not trust\n";
    private const string Executive = "[EXEC    ] - [OK  ] - executive started, pid 1\n";
    private const string Panic =
        "*** KERNEL PANIC ***\nInvariant violated: no esp0 stack for thread 0 (PROCESS_MODEL.md section 8)\n" +
        "Called from EIP: 0xC0100317\nSystem halted.";

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_summary_passes_and_carries_the_count()
    {
        var v = BootVerdict.Classify(Refusal + Summary, full: false);
        Assert.Equal(BootOutcome.Pass, v.Outcome);
        Assert.Equal(45, v.TestsPassed);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_panic_fails_even_after_a_summary()
    {
        var v = BootVerdict.Classify(Summary + Executive + Panic, full: true);
        Assert.Equal(BootOutcome.Fail, v.Outcome);
        Assert.Contains("no esp0 stack", v.Reason);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Failed_self_tests_fail()
    {
        var v = BootVerdict.Classify("[KTEST   ] - [FAIL] - esp0 follows the running thread\n" +
                                     "[KTEST   ] - [FAIL] - self-tests failed: 1 failure(s)\n", full: false);
        Assert.Equal(BootOutcome.Fail, v.Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_self_tests_deliberate_refusals_are_not_failures()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify(Refusal, full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Silence_is_inconclusive_never_a_pass()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify("", full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_full_boot_also_needs_the_executive()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify(Summary, full: true).Outcome);
        Assert.Equal(BootOutcome.Pass, BootVerdict.Classify(Summary + Executive, full: true).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Verification_boots_never_write_to_the_image()
    {
        // Booting a release image without -snapshot once broke its SHA256SUMS.
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log"));
        Assert.Contains("-snapshot", args);
        Assert.Contains("pc", args);   // q35 makes the ATA self-test vacuous
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_cpu_model_reaches_qemu_only_when_asked_for()
    {
        // QEMU's default model unless one is named; a named one as -cpu, so
        // the minimal and the full model boots really differ.
        var plain = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log"));
        Assert.DoesNotContain("-cpu", plain);
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log", "pentium"));
        int i = args.IndexOf("-cpu");
        Assert.True(i >= 0 && i + 1 < args.Count);
        Assert.Equal("pentium", args[i + 1]);
    }
}
