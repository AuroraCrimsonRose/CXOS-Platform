// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// <c>cxk verify-hyperv</c> drives Hyper-V through a PowerShell script. The boot
/// itself needs Hyper-V and is checked by running it; what these pin is that no
/// name or path reaches that script as anything but a quoted value.
/// </summary>
public class HyperVBootTests
{
    private static readonly HyperVBoot.Options Defaults = new();

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_vm_name_is_safe_in_a_script()
    {
        string name = HyperVBoot.NewVmName();
        Assert.StartsWith("cxk-verify-", name);
        Assert.All(name, c => Assert.True(char.IsAsciiLetterOrDigit(c) || c == '-'));
        Assert.NotEqual(name, HyperVBoot.NewVmName());
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_path_is_single_quoted_with_its_quotes_doubled()
    {
        string s = HyperVBoot.CreateScript("cxk-verify-1", @"C:\Temp\o'brien", @"C:\Temp\o'brien\$(evil).vhdx", "cxk-verify-1", Defaults);
        Assert.Contains(@"-Path 'C:\Temp\o''brien'", s);
        Assert.Contains(@"-Path 'C:\Temp\o''brien\$(evil).vhdx'", s);
        Assert.Contains("-Generation 1", s);
        Assert.Contains("-ControllerType IDE", s);
        Assert.Contains(@"'\\.\pipe\cxk-verify-1'", s);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("x'; Remove-VM *; '")]
    [InlineData("a b")]
    [InlineData("")]
    public void An_unsafe_name_is_refused(string bad)
    {
        Assert.Throws<ArgumentException>(() => HyperVBoot.CreateScript(bad, "C:\\t", "C:\\t\\d.vhdx", "p", Defaults));
        Assert.Throws<ArgumentException>(() => HyperVBoot.CreateScript("vm", "C:\\t", "C:\\t\\d.vhdx", bad, Defaults));
        Assert.Throws<ArgumentException>(() => HyperVBoot.RemoveScript(bad));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Memory_and_processors_are_bounded()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            HyperVBoot.CreateScript("vm", "C:\\t", "C:\\t\\d.vhdx", "p", new HyperVBoot.Options(MemoryMb: 100)));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            HyperVBoot.CreateScript("vm", "C:\\t", "C:\\t\\d.vhdx", "p", new HyperVBoot.Options(Processors: 0)));
    }
}
