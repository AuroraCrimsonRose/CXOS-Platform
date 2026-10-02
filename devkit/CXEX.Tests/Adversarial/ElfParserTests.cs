using CXEX.Build.Parsers;
using Xunit;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// The ELF cases of DevKit security review §14, one test each.
///
/// <para>These need nothing but .NET - no toolchain, no checkout, no built OS - so
/// they run on every host and can gate every push. That is deliberate: the ELF
/// boundary is where untrusted input first meets the DevKit, and a check that only
/// runs on one developer's Linux box is not a boundary.</para>
/// </summary>
public class ElfParserTests
{
    private static void Rejects(byte[] elf, string because)
    {
        Exception ex = Record.Exception(() => ElfParser.Parse(elf))!;

        Assert.True(ex is not null, $"ElfParser accepted a malformed ELF: {because}");

        // The type matters as much as the rejection. An IndexOutOfRange or an
        // ArgumentOutOfRange means the parser fell off the end of the file rather
        // than deciding anything - which is exactly the pre-hardening behaviour
        // this suite exists to prevent coming back.
        Assert.True(
            ex is InvalidDataException or NotSupportedException,
            $"expected a considered rejection for {because}, got {ex!.GetType().Name}: {ex.Message}");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_well_formed_elf_is_accepted()
    {
        // Without this the suite could pass by rejecting everything.
        ElfImage img = ElfParser.Parse(Elf32Builder.Valid().Build());

        Assert.Equal(0x00401000u, img.EntryPoint);
        ElfSegment only = Assert.Single(img.Segments);
        Assert.Equal(64u, only.FileSize);
        Assert.Equal(64, only.Data.Length);
        Assert.True(only.IsExecutable);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Truncated_header_is_rejected()
    {
        byte[] full = Elf32Builder.Valid().Build();
        for (int len = 0; len < Elf32Builder.HeaderSize; len += 13)
            Rejects(full.AsSpan(0, len).ToArray(), $"only {len} bytes of a 52-byte header");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Bad_magic_is_rejected()
    {
        byte[] elf = Elf32Builder.Valid().Build();
        elf[1] = (byte)'X';
        Rejects(elf, "corrupted \\x7FELF magic");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Wrong_class_or_endianness_is_rejected()
    {
        Rejects(Elf32Builder.Valid().With(b => b.Class = 2).Build(), "ELFCLASS64");
        Rejects(Elf32Builder.Valid().With(b => b.Data = 2).Build(), "big-endian ELF");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Non_executable_elf_type_is_rejected()
    {
        Rejects(Elf32Builder.Valid().With(b => b.Type = 1).Build(), "ET_REL, a relocatable object");
        Rejects(Elf32Builder.Valid().With(b => b.Type = 3).Build(), "ET_DYN, a shared image");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Wrong_machine_is_rejected() =>
        Rejects(Elf32Builder.Valid().With(b => b.Machine = 62).Build(), "EM_X86_64, not EM_386");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Invalid_program_header_size_is_rejected()
    {
        Rejects(Elf32Builder.Valid().With(b => b.PhEntSize = 0).Build(), "e_phentsize 0");
        Rejects(Elf32Builder.Valid().With(b => b.PhEntSize = 31).Build(), "e_phentsize below 32");
        Rejects(Elf32Builder.Valid().With(b => b.PhEntSize = 56).Build(), "e_phentsize of an ELF64 header");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Program_header_table_outside_the_file_is_rejected()
    {
        Rejects(Elf32Builder.Valid().With(b => b.PhOff = 0x7FFF_0000).Build(),
            "e_phoff far past the end of the file");

        // The sum, not the base, is what leaves the file - the classic way a table
        // looks like it starts inside one.
        Rejects(Elf32Builder.Valid().With(b => b.PhNumOverride = 4096).Build(),
            "e_phoff inside the file but the table running off the end");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Program_header_count_overflow_is_rejected()
    {
        // 0xFFFFFF00 + 8 * 32 is exactly 2^32, which is 0 in 32-bit arithmetic - so
        // a narrow bounds check sees a table starting at the end of memory and
        // ending at the start of the file, and waves it through. The count stays
        // under the absurd-count cap on purpose, so this test can only pass by way
        // of the overflow check it is actually aimed at.
        Rejects(Elf32Builder.Valid().With(b =>
        {
            b.PhOff = 0xFFFF_FF00;
            b.PhNumOverride = 8;
        }).Build(), "e_phoff + e_phnum * e_phentsize wrapping 32 bits");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Absurd_segment_count_is_rejected() =>
        Rejects(Elf32Builder.Valid().With(b => b.PhNumOverride = 0xFFFF).Build(),
            "65535 program headers");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Offset_plus_size_overflow_is_rejected()
    {
        // The original defect: both casts go negative at span.Slice((int)o,(int)n).
        Rejects(Elf32Builder.Valid().WithSegment(0, p =>
        {
            p.Offset = 0xFFFF_FFF0;
            p.FileSize = 0x1000;
            p.MemSize = 0x1000;
            p.Payload = Array.Empty<byte>();
        }).Build(), "p_offset + p_filesz wrapping past the end of the file");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Segment_running_past_the_end_of_the_file_is_rejected() =>
        Rejects(Elf32Builder.Valid().WithSegment(0, p =>
        {
            p.FileSize = 0x10_0000;
            p.MemSize = 0x10_0000;
            p.Payload = Array.Empty<byte>();
        }).Build(), "p_filesz claiming a megabyte that is not there");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Filesz_greater_than_memsz_is_rejected() =>
        Rejects(Elf32Builder.Valid().WithSegment(0, p => p.MemSize = p.FileSize - 1).Build(),
            "p_filesz greater than p_memsz");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Virtual_address_overflow_is_rejected() =>
        Rejects(Elf32Builder.Valid().WithSegment(0, p =>
        {
            p.Vaddr = 0xFFFF_F000;
            p.MemSize = 0x8000;
        }).Build(), "p_vaddr + p_memsz leaving the 32-bit address space");

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Absurd_aggregate_memory_size_is_rejected()
    {
        var b = Elf32Builder.Valid();
        // Each is individually plausible; together they claim 3 GB.
        for (int i = 0; i < 3; i++)
        {
            b.Segments.Add(new Elf32Builder.Phdr
            {
                Offset = 0x1000,
                Vaddr = (uint)(0x1000_0000 + i * 0x4000_0000L) & 0xFFFF_F000,
                MemSize = 0x4000_0000,
                FileSize = 0,
                Flags = 0x6,
                Align = 0x1000,
            });
        }
        Rejects(b.Build(), "segments totalling gigabytes of mapped memory");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Invalid_alignment_is_rejected()
    {
        Rejects(Elf32Builder.Valid().WithSegment(0, p => p.Align = 0x1800).Build(),
            "p_align that is not a power of two");

        Rejects(Elf32Builder.Valid().WithSegment(0, p => p.Vaddr += 1).Build(),
            "p_vaddr and p_offset disagreeing modulo p_align");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Overlapping_loadable_segments_are_rejected()
    {
        var b = Elf32Builder.Valid();
        b.Segments.Add(new Elf32Builder.Phdr
        {
            Offset = 0x1000,
            Vaddr = b.Segments[0].Vaddr + 0x10,   // starts inside segment 0
            FileSize = 0,
            MemSize = 0x100,
            Flags = 0x6,
            Align = 0x10,
        });
        Rejects(b.Build(), "two PT_LOAD segments overlapping in memory");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Invalid_entry_point_is_rejected()
    {
        Rejects(Elf32Builder.Valid().With(b => b.EntryPoint = 0xDEAD_B000).Build(),
            "an entry point in no segment at all");

        // Inside a mapped segment, but one that is not executable.
        var b2 = Elf32Builder.Valid();
        b2.Segments[0].Flags = 0x6;               // R + W, no X
        Rejects(b2.Build(), "an entry point in a non-executable segment");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void No_loadable_segments_is_rejected() =>
        Rejects(Elf32Builder.Valid().WithSegment(0, p => p.Type = 4 /* PT_NOTE */).Build(),
            "a file whose only program header is not PT_LOAD");
}
