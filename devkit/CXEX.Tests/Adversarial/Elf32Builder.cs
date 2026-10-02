using CXEX.Core.Utilities;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// Builds a minimal, well-formed 32-bit ELF executable, so each adversarial test
/// can state its case as one mutation of something known good.
///
/// <para>Hand-written rather than taken from the build output on purpose: a test
/// that reads <c>build/kernel.elf</c> only runs where the OS has been built, and
/// silently changes meaning whenever the kernel does. These bytes are fixed and
/// run anywhere .NET does, which is what keeps the Adversarial category free of
/// the Toolchain requirement.</para>
/// </summary>
public sealed class Elf32Builder
{
    public const int HeaderSize = 52;
    public const int PhdrSize = 32;

    public ushort Type { get; set; } = 2;        // ET_EXEC
    public ushort Machine { get; set; } = 3;     // EM_386
    public byte Class { get; set; } = 1;         // ELFCLASS32
    public byte Data { get; set; } = 1;          // ELFDATA2LSB
    public byte IdentVersion { get; set; } = 1;
    public uint EntryPoint { get; set; } = 0x00400000;
    public uint PhOff { get; set; } = HeaderSize;
    public ushort PhEntSize { get; set; } = PhdrSize;

    /// <summary>Overrides e_phnum. Null means "however many segments there are".</summary>
    public ushort? PhNumOverride { get; set; }

    public List<Phdr> Segments { get; } = new();

    public sealed class Phdr
    {
        public uint Type = 1;                     // PT_LOAD
        public uint Offset;
        public uint Vaddr;
        public uint Paddr;
        public uint FileSize;
        public uint MemSize;
        public uint Flags = 0x5;                  // R + X
        public uint Align = 0x1000;
        public byte[] Payload = Array.Empty<byte>();
    }

    /// <summary>
    /// One executable segment at 0x00400000 holding <paramref name="codeBytes"/>
    /// bytes, laid out so p_vaddr and p_offset agree modulo the page size.
    /// </summary>
    public static Elf32Builder Valid(int codeBytes = 64)
    {
        var b = new Elf32Builder();
        b.Segments.Add(new Phdr
        {
            Offset = 0x1000,
            Vaddr = 0x00400000 + 0x1000,
            Paddr = 0x00400000 + 0x1000,
            FileSize = (uint)codeBytes,
            MemSize = (uint)codeBytes,
            Flags = 0x5,
            Align = 0x1000,
            Payload = Enumerable.Range(0, codeBytes).Select(i => (byte)(0x90 + (i & 7))).ToArray(),
        });
        b.EntryPoint = 0x00400000 + 0x1000;
        return b;
    }

    public Elf32Builder With(Action<Elf32Builder> mutate)
    {
        mutate(this);
        return this;
    }

    /// <summary>Mutates segment <paramref name="index"/>, for the single-field adversarial cases.</summary>
    public Elf32Builder WithSegment(int index, Action<Phdr> mutate)
    {
        mutate(Segments[index]);
        return this;
    }

    public byte[] Build()
    {
        // The file is sized from the header, the natural table position and the
        // payloads - deliberately NOT from PhOff. Sizing it to cover an arbitrary
        // PhOff would quietly repair the very malformation a test is expressing:
        // "e_phoff past the end of the file" became "a 2 GB file whose table is
        // exactly in bounds", and the parser was right to accept it.
        int end = HeaderSize + Segments.Count * PhdrSize;
        foreach (Phdr p in Segments)
        {
            if (p.Payload.Length == 0) continue;
            long stop = (long)p.Offset + p.Payload.Length;
            if (stop > end && stop < 1 << 20) end = (int)stop;
        }

        byte[] file = new byte[end];
        Span<byte> span = file;

        // e_ident
        span[0] = 0x7F; span[1] = (byte)'E'; span[2] = (byte)'L'; span[3] = (byte)'F';
        span[4] = Class;
        span[5] = Data;
        span[6] = IdentVersion;

        MemoryPrimitives.WriteU16(span, 16, Type);
        MemoryPrimitives.WriteU16(span, 18, Machine);
        MemoryPrimitives.WriteU32(span, 20, 1);              // e_version
        MemoryPrimitives.WriteU32(span, 24, EntryPoint);
        MemoryPrimitives.WriteU32(span, 28, PhOff);
        MemoryPrimitives.WriteU16(span, 40, HeaderSize);     // e_ehsize
        MemoryPrimitives.WriteU16(span, 42, PhEntSize);
        MemoryPrimitives.WriteU16(span, 44, PhNumOverride ?? (ushort)Segments.Count);

        for (int i = 0; i < Segments.Count; i++)
        {
            // A deliberately out-of-range PhOff is left unwritten rather than
            // clamped. The negative check matters: PhOff near 0xFFFFFFFF casts to a
            // negative int, which slips past an upper-bound test on its own.
            long at64 = (long)PhOff + (long)i * PhEntSize;
            if (at64 < 0 || at64 + PhdrSize > file.Length) break;
            int at = (int)at64;

            Phdr p = Segments[i];
            MemoryPrimitives.WriteU32(span, at + 0, p.Type);
            MemoryPrimitives.WriteU32(span, at + 4, p.Offset);
            MemoryPrimitives.WriteU32(span, at + 8, p.Vaddr);
            MemoryPrimitives.WriteU32(span, at + 12, p.Paddr);
            MemoryPrimitives.WriteU32(span, at + 16, p.FileSize);
            MemoryPrimitives.WriteU32(span, at + 20, p.MemSize);
            MemoryPrimitives.WriteU32(span, at + 24, p.Flags);
            MemoryPrimitives.WriteU32(span, at + 28, p.Align);

            if (p.Payload.Length > 0 && p.Offset + p.Payload.Length <= file.Length)
                p.Payload.CopyTo(span.Slice((int)p.Offset));
        }

        return file;
    }
}
