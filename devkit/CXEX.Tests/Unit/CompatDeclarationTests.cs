// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.RegularExpressions;
using CXEX.Build.Engines;
using CXEX.Build.Parsers;
using CXEX.Core.Constants;
using CXEX.Lang.Abi;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The kernel's loader now calls <c>cxex_check_compat</c>, and requires every
/// image to declare that it wants to be checked: an image without both
/// REQUIRE_* flags is refused with <c>CXEX_LOAD_NO_COMPAT</c>, and one whose
/// <c>abi_version</c> differs from <c>CXK_ABI_VERSION</c> with
/// <c>CXEX_LOAD_BAD_ABI</c>.
///
/// <para>That makes two properties of this writer load-bearing which were
/// previously only conventional, and a break in either produces an image that
/// builds, signs and stages cleanly and then refuses to run. The boot would
/// catch it, but by then the only evidence is one line from the loader, so it
/// is stated here where the cause is.</para>
///
/// <para><c>cxk check-versions</c> already ties <c>AbiVersion</c> to
/// <c>versions.json</c>. This ties it to the header the kernel actually
/// compiles against, which is the number the comparison uses.</para>
/// </summary>
public class CompatDeclarationTests
{
    static ElfSegment Segment() => new()
    {
        Vaddr = 0x00400000,
        Paddr = 0x00400000,
        FileSize = 16,
        MemSize = 16,
        Flags = 0x5,                       // R + X
        Data = new byte[16],
    };

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData(CXFlags.TYPE_KERNEL)]
    [InlineData(CXFlags.TYPE_OS)]
    [InlineData(CXFlags.TYPE_SYSTEM)]
    [InlineData(CXFlags.TYPE_USER)]
    public void Every_image_type_asks_to_be_refused_on_a_mismatch(ushort typeCode)
    {
        var layout = CXEXLayoutEngine.CreateLayout(0x00400000, new[] { Segment() }, typeCode, AbiModel.EmbeddedFile.Hash);

        // Both, separately. One assertion over the pair would pass a writer
        // that set only one, and the kernel refuses on either being absent.
        Assert.True((layout.Flags & CXFlags.FLAG_REQUIRE_ABI_MATCH) != 0,
            $"type 0x{typeCode:X4} does not set FLAG_REQUIRE_ABI_MATCH: the kernel will refuse it");
        Assert.True((layout.Flags & CXFlags.FLAG_REQUIRE_ARCH_MATCH) != 0,
            $"type 0x{typeCode:X4} does not set FLAG_REQUIRE_ARCH_MATCH: the kernel will refuse it");

        // Bit 3 was KERNEL_PRIV: a privilege an image claimed for itself and that
        // nothing read. Retired 2026-10-16; no writer sets it, kernel images included.
        Assert.True((layout.Flags & CXFlags.FLAG_RETIRED_3) == 0,
            $"type 0x{typeCode:X4} sets retired bit 3 (was KERNEL_PRIV)");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_stamped_abi_version_is_the_one_the_kernel_compiles_against()
    {
        string root = Requires.Repo();
        string header = File.ReadAllText(Path.Combine(root, "abi", "cxk_abi.h"));

        Match m = Regex.Match(header, @"^#define\s+CXK_ABI_VERSION\s+(\d+)",
                              RegexOptions.Multiline);
        Assert.True(m.Success, "CXK_ABI_VERSION not found in abi/cxk_abi.h");

        Assert.Equal(ushort.Parse(m.Groups[1].Value), CXEXLayoutEngine.AbiVersion);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_arch_numbers_are_the_ones_the_kernel_compares()
    {
        // FLAG_REQUIRE_ARCH_MATCH makes these load-bearing as the ABI version is:
        // stamp x86-64 with the wrong number and every x86-64 program is refused
        // with CXEX_LOAD_BAD_ARCH.
        string root = Requires.Repo();
        string header = File.ReadAllText(Path.Combine(root, "kernel", "lib", "format", "cxex.h"));
        foreach (var (name, value) in new[] { ("CXEX_ARCH_X86_32", CXFlags.ARCH_X86_32),
                                              ("CXEX_ARCH_X86_64", CXFlags.ARCH_X86_64) })
        {
            Match m = Regex.Match(header, $@"^#define\s+{name}\s+(\d+)u?\b", RegexOptions.Multiline);
            Assert.True(m.Success, $"{name} not found in kernel/lib/format/cxex.h");
            Assert.Equal(ushort.Parse(m.Groups[1].Value), value);
        }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_layout_carries_the_arch_it_is_given_and_refuses_any_other()
    {
        // x86-32 when unsaid, which is every caller that predates x86-64.
        Assert.Equal(CXFlags.ARCH_X86_32, CXEXLayoutEngine.CreateLayout(0x00400000, new[] { Segment() },
            CXFlags.TYPE_USER, AbiModel.EmbeddedFile.Hash).ArchTarget);
        Assert.Equal(CXFlags.ARCH_X86_64, CXEXLayoutEngine.CreateLayout(0x00400000, new[] { Segment() },
            CXFlags.TYPE_USER, AbiModel.EmbeddedFile.Hash, arch: CXFlags.ARCH_X86_64).ArchTarget);
        foreach (ushort bad in new ushort[] { 0, 3, 0xFFFF })
            Assert.Throws<InvalidDataException>(() => CXEXLayoutEngine.CreateLayout(0x00400000, new[] { Segment() },
                CXFlags.TYPE_USER, AbiModel.EmbeddedFile.Hash, arch: bad));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_unspecified_abi_version_is_the_current_contract_not_zero()
    {
        // The parameter has a default, so a caller that omits it stamps
        // AbiVersion rather than 0 - and 0 is the value a kernel would refuse
        // while the image looked, in every other respect, well formed.
        var layout = CXEXLayoutEngine.CreateLayout(0x00400000, new[] { Segment() },
                                                   CXFlags.TYPE_USER, AbiModel.EmbeddedFile.Hash);
        Assert.Equal(CXEXLayoutEngine.AbiVersion, layout.AbiVersion);
        Assert.NotEqual(0, layout.AbiVersion);
    }
}
