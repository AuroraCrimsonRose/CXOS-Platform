// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Core.Constants;
using CXEX.FileType.Types;
using Xunit;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// The CXEX cases of DevKit security review §14, against
/// <see cref="CXEXExecutable.Load"/> - the point where an untrusted image becomes
/// a model the rest of the DevKit acts on.
/// </summary>
public class CxexLoadTests
{
    private static void Rejects(byte[] image, string because)
    {
        var exe = new CXEXExecutable();
        Exception? ex = Record.Exception(() => exe.Load(image));

        Assert.True(ex is not null, $"Load accepted a malformed CXEX: {because}");

        // As with the ELF suite: an IndexOutOfRange means Load walked off the end of
        // the file rather than deciding anything, which is the behaviour being retired.
        Assert.True(
            ex is InvalidDataException,
            $"expected a considered rejection for {because}, got {ex!.GetType().Name}: {ex.Message}");
    }

    private static CXEXExecutable Loads(byte[] image)
    {
        var exe = new CXEXExecutable();
        exe.Load(image);
        return exe;
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_well_formed_image_is_accepted()
    {
        // Without this the suite could pass by rejecting everything.
        CXEXExecutable exe = Loads(CxexBuilder.Valid().Build());

        CXEXSectionAssert(exe);
        static void CXEXSectionAssert(CXEXExecutable e)
        {
            var only = Assert.Single(e.Sections);
            Assert.Equal(".text", only.Name);
            Assert.Equal(0x00400000u, only.VirtAddr);
            Assert.Equal(64u, only.FileSize);
            Assert.Equal(64, e.GetSectionData(".text").Length);
            Assert.Null(e.Signature);
        }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Truncated_or_non_cxex_input_is_rejected()
    {
        Rejects(Array.Empty<byte>(), "an empty file");
        Rejects(new byte[32], "fewer bytes than a 56-byte header");

        byte[] img = CxexBuilder.Valid().Build();
        // Byte 0, not byte 1: the magic reads 'C','X','E','X', so overwriting
        // index 1 with 'X' changes nothing and the case tests itself instead.
        img[0] = (byte)'Z';
        Rejects(img, "corrupted CXEX magic");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Unsupported_format_version_is_rejected() =>
        Rejects(CxexBuilder.Valid().With(b => b.FormatVersion = 2).Build(),
            "a format version whose field offsets this cannot assume");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Section_count_overflow_is_rejected()
    {
        Rejects(CxexBuilder.Valid().With(b => b.SectionCountOverride = 0xFFFF).Build(),
            "65535 sections");

        // Under the absurd-count cap, so this can only pass by way of the bounds
        // check on section_offset + count * 28.
        Rejects(CxexBuilder.Valid().With(b => b.SectionCountOverride = 200).Build(),
            "a section table running past the end of the file");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Section_table_overlapping_the_header_is_rejected() =>
        Rejects(CxexBuilder.Valid().With(b => b.SectionOffsetOverride = 8).Build(),
            "a section table starting inside the 56-byte header");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void File_offset_overflow_is_rejected()
    {
        Rejects(CxexBuilder.Valid().WithSection(0, s =>
        {
            s.FileOffset = 0xFFFF_FFF0;
            s.Payload = Array.Empty<byte>();
        }).Build(), "file_offset + file_size wrapping past the end of the file");

        Rejects(CxexBuilder.Valid().WithSection(0, s =>
        {
            s.FileSize = 0x10_0000;
            s.MemSize = 0x10_0000;
            s.Payload = Array.Empty<byte>();
        }).Build(), "file_size claiming a megabyte that is not there");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Section_data_inside_the_section_table_is_rejected() =>
        Rejects(CxexBuilder.Valid().WithSection(0, s => s.FileOffset = 60).Build(),
            "section bytes overlapping the header or section table");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void File_size_greater_than_mem_size_is_rejected() =>
        Rejects(CxexBuilder.Valid().WithSection(0, s => s.MemSize = s.FileSize - 1).Build(),
            "file_size greater than mem_size");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Virtual_address_overflow_is_rejected() =>
        Rejects(CxexBuilder.Valid().WithSection(0, s =>
        {
            s.VirtualAddress = 0xFFFF_F000;
            s.MemSize = 0x8000;
        }).Build(), "virt_addr + mem_size leaving the 32-bit address space");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Overlapping_sections_are_rejected()
    {
        var b = CxexBuilder.Valid();
        b.Sections.Add(new CxexBuilder.Section
        {
            Name = ".data",
            FileOffset = 0,
            VirtualAddress = b.Sections[0].VirtualAddress + 0x10,   // starts inside .text
            FileSize = 0,
            MemSize = 0x100,
            Flags = CXFlags.SEC_READ | CXFlags.SEC_WRITE,
        });
        Rejects(b.Build(), "two sections overlapping in memory");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Disagreement_between_flag_signed_and_signature_offset_is_rejected()
    {
        Rejects(CxexBuilder.Valid().With(b => b.Flags = CXFlags.FLAG_SIGNED).Build(),
            "FLAG_SIGNED set with signature_offset 0");

        Rejects(CxexBuilder.Valid().With(b => b.SignatureOffset = 120).Build(),
            "a signature_offset with FLAG_SIGNED clear");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Signature_offset_past_the_end_is_rejected() =>
        Rejects(CxexBuilder.Valid().With(b =>
        {
            b.Flags = CXFlags.FLAG_SIGNED;
            b.SignatureOffset = 0xFFFF_FFF0;
        }).Build(), "signature_offset past the end of the file");

    /// <summary>
    /// The Critical finding both reviews raise, and the one the plan adds in its own
    /// words: the kernel hashes <c>[0, signature_offset)</c>, but nothing required a
    /// section's bytes to lie inside that range. Data placed after the signature is
    /// mapped by the loader and covered by nothing.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Section_data_beyond_the_signed_range_is_rejected()
    {
        var b = CxexBuilder.Valid();
        b.Flags = CXFlags.FLAG_SIGNED;
        // The signature starts immediately after the section table, so the section's
        // own 64 bytes now sit beyond the signed range entirely.
        b.SignatureOffset = (uint)(CxexBuilder.HeaderSize + CxexBuilder.SectionEntrySize);

        Rejects(b.Build(), "a section whose bytes lie past signature_offset, so the signature does not cover them");
    }

    /// <summary>
    /// Load proves the range is inside the file, but Sections is public and mutable,
    /// so the accessor must not trust its own model either.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void GetSectionData_refuses_a_section_pointing_outside_the_image()
    {
        CXEXExecutable exe = Loads(CxexBuilder.Valid().Build());
        exe.Sections[0].FileOffset = 0xFFFF_FFF0;

        Assert.Throws<InvalidDataException>(() => exe.GetSectionData(".text"));
    }
}
