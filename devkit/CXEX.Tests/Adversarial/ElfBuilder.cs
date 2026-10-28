// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Core.Utilities;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// Builds a minimal, well-formed ELF executable, 32-bit or (with
/// <see cref="Wide"/>) 64-bit, so each adversarial test can state its case as
/// one mutation of something known good.
///
/// <para>Hand-written rather than taken from the build output on purpose: a test
/// that reads <c>build/kernel.elf</c> only runs where the OS has been built, and
/// silently changes meaning whenever the kernel does. These bytes are fixed and
/// run anywhere .NET does, which is what keeps the Adversarial category free of
/// the Toolchain requirement.</para>
///
/// <para><see cref="Wide"/> picks the layout the bytes are written in; <see cref="Class"/>
/// and <see cref="Machine"/> are only what the header claims, so a test can
/// write one layout under the other's class.</para>
/// </summary>
public sealed class ElfBuilder
{
    public const int HeaderSize = 52;
    public const int PhdrSize = 32;
    public const int HeaderSize64 = 64;
    public const int PhdrSize64 = 56;

    /// <summary>Writes the ELF64 layout (header 64 bytes, program headers 56).</summary>
    public bool Wide { get; set; }

    public ushort Type { get; set; } = 2;        // ET_EXEC
    public ushort Machine { get; set; } = 3;     // EM_386
    public byte Class { get; set; } = 1;         // ELFCLASS32
    public byte Data { get; set; } = 1;          // ELFDATA2LSB
    public byte IdentVersion { get; set; } = 1;
    public ulong EntryPoint { get; set; } = 0x00400000;
    public ulong PhOff { get; set; } = HeaderSize;
    public ushort PhEntSize { get; set; } = PhdrSize;

    /// <summary>Overrides e_phnum. Null means "however many segments there are".</summary>
    public ushort? PhNumOverride { get; set; }

    public List<Phdr> Segments { get; } = new();

    public sealed class Phdr
    {
        public uint Type = 1;                     // PT_LOAD
        public ulong Offset;
        public ulong Vaddr;
        public ulong Paddr;
        public ulong FileSize;
        public ulong MemSize;
        public uint Flags = 0x5;                  // R + X
        public ulong Align = 0x1000;
        public byte[] Payload = Array.Empty<byte>();
    }

    /// <summary>
    /// One executable segment at 0x00401000 holding <paramref name="codeBytes"/>
    /// bytes, laid out so p_vaddr and p_offset agree modulo the page size.
    /// </summary>
    public static ElfBuilder Valid(int codeBytes = 64)
    {
        var b = new ElfBuilder();
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

    /// <summary><see cref="Valid"/> as an x86-64 ELF64: the same segment and entry point.</summary>
    public static ElfBuilder Valid64(int codeBytes = 64)
    {
        var b = Valid(codeBytes);
        b.Wide = true;
        b.Class = 2;                              // ELFCLASS64
        b.Machine = 62;                           // EM_X86_64
        b.PhOff = HeaderSize64;
        b.PhEntSize = PhdrSize64;
        return b;
    }

    public ElfBuilder With(Action<ElfBuilder> mutate)
    {
        mutate(this);
        return this;
    }

    /// <summary>Mutates segment <paramref name="index"/>, for the single-field adversarial cases.</summary>
    public ElfBuilder WithSegment(int index, Action<Phdr> mutate)
    {
        mutate(Segments[index]);
        return this;
    }

    public byte[] Build()
    {
        int headerSize = Wide ? HeaderSize64 : HeaderSize;
        int phdrSize = Wide ? PhdrSize64 : PhdrSize;

        // The file is sized from the header, the natural table position and the
        // payloads - deliberately NOT from PhOff. Sizing it to cover an arbitrary
        // PhOff would quietly repair the very malformation a test is expressing:
        // "e_phoff past the end of the file" became "a 2 GB file whose table is
        // exactly in bounds", and the parser was right to accept it.
        int end = headerSize + Segments.Count * phdrSize;
        foreach (Phdr p in Segments)
        {
            if (p.Payload.Length == 0) continue;
            ulong stop = p.Offset + (ulong)p.Payload.Length;
            if (stop > (ulong)end && stop < 1 << 20) end = (int)stop;
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
        if (Wide)
        {
            MemoryPrimitives.WriteU64(span, 24, EntryPoint);
            MemoryPrimitives.WriteU64(span, 32, PhOff);
            MemoryPrimitives.WriteU16(span, 52, HeaderSize64);   // e_ehsize
            MemoryPrimitives.WriteU16(span, 54, PhEntSize);
            MemoryPrimitives.WriteU16(span, 56, PhNumOverride ?? (ushort)Segments.Count);
        }
        else
        {
            MemoryPrimitives.WriteU32(span, 24, (uint)EntryPoint);
            MemoryPrimitives.WriteU32(span, 28, (uint)PhOff);
            MemoryPrimitives.WriteU16(span, 40, HeaderSize);     // e_ehsize
            MemoryPrimitives.WriteU16(span, 42, PhEntSize);
            MemoryPrimitives.WriteU16(span, 44, PhNumOverride ?? (ushort)Segments.Count);
        }

        for (int i = 0; i < Segments.Count; i++)
        {
            // A deliberately out-of-range PhOff is left unwritten rather than
            // clamped: the table is only written where it fits in the file.
            ulong at64 = PhOff + (ulong)i * PhEntSize;
            if (at64 + (ulong)phdrSize > (ulong)file.Length) break;
            int at = (int)at64;

            Phdr p = Segments[i];
            MemoryPrimitives.WriteU32(span, at + 0, p.Type);
            if (Wide)
            {
                MemoryPrimitives.WriteU32(span, at + 4, p.Flags);
                MemoryPrimitives.WriteU64(span, at + 8, p.Offset);
                MemoryPrimitives.WriteU64(span, at + 16, p.Vaddr);
                MemoryPrimitives.WriteU64(span, at + 24, p.Paddr);
                MemoryPrimitives.WriteU64(span, at + 32, p.FileSize);
                MemoryPrimitives.WriteU64(span, at + 40, p.MemSize);
                MemoryPrimitives.WriteU64(span, at + 48, p.Align);
            }
            else
            {
                MemoryPrimitives.WriteU32(span, at + 4, (uint)p.Offset);
                MemoryPrimitives.WriteU32(span, at + 8, (uint)p.Vaddr);
                MemoryPrimitives.WriteU32(span, at + 12, (uint)p.Paddr);
                MemoryPrimitives.WriteU32(span, at + 16, (uint)p.FileSize);
                MemoryPrimitives.WriteU32(span, at + 20, (uint)p.MemSize);
                MemoryPrimitives.WriteU32(span, at + 24, p.Flags);
                MemoryPrimitives.WriteU32(span, at + 28, (uint)p.Align);
            }

            if (p.Payload.Length > 0 && p.Offset + (ulong)p.Payload.Length <= (ulong)file.Length)
                p.Payload.CopyTo(span.Slice((int)p.Offset));
        }

        return file;
    }
}
