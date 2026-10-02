// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using CXEX.Core.Utilities;

namespace CXEX.Build.Parsers;

public class ElfSegment
{
    public uint Offset { get; set; }
    public uint Vaddr { get; set; }
    public uint Paddr { get; set; }
    public uint FileSize { get; set; }
    public uint MemSize { get; set; }
    public uint Align { get; set; }
    public uint Flags { get; set; }
    public byte[] Data { get; set; } = Array.Empty<byte>();

    /// <summary>One past the last virtual address this segment occupies, as 64-bit so it cannot wrap.</summary>
    public ulong VirtualEnd => (ulong)Vaddr + MemSize;

    public bool IsExecutable => (Flags & 0x1) != 0;
    public bool IsWritable => (Flags & 0x2) != 0;
}

/// <summary>
/// An ELF that has passed every structural check in <see cref="ElfParser"/>.
///
/// <para>The type is the guarantee: holding one means the header was well formed,
/// every program header lay inside the file, no arithmetic wrapped, no two loadable
/// segments overlapped, and the entry point lands in executable code. Code
/// downstream - the CXEX layout engine, the writer, the signer - may rely on that
/// instead of re-deriving it, which is the "parse, validate, transform, emit"
/// pipeline the DevKit engineering review asks for (§3).</para>
/// </summary>
public sealed class ElfImage
{
    public uint EntryPoint { get; }

    /// <summary>The PT_LOAD segments, in file order. Never empty.</summary>
    public IReadOnlyList<ElfSegment> Segments { get; }

    internal ElfImage(uint entryPoint, IReadOnlyList<ElfSegment> segments)
    {
        EntryPoint = entryPoint;
        Segments = segments;
    }

    /// <summary>So existing call sites can keep destructuring the result.</summary>
    public void Deconstruct(out uint entryPoint, out IReadOnlyList<ElfSegment> segments)
    {
        entryPoint = EntryPoint;
        segments = Segments;
    }
}

/// <summary>
/// Reads a 32-bit little-endian ELF executable and validates it completely before
/// returning anything (DevKit security review §1, engineering review §1).
///
/// <para>This used to check the magic, the class and the endianness, and nothing
/// else - then slice the file with attacker-supplied offsets. A header claiming a
/// segment at offset 0xFFFFFFF0 of length 0x1000 reached
/// <c>span.Slice((int)offset, (int)size)</c>, where both casts go negative and the
/// slice throws something arbitrary from deep inside the BCL, if it does not read
/// adjacent memory first. Being C# bounded the damage, but malformed input still
/// reached layout code, which is the boundary this closes.</para>
///
/// <para>Every rejection below throws <see cref="InvalidDataException"/> naming the
/// field and the value, because the usual reader of this message is someone holding
/// a linker script that produced something unexpected, not an attacker.</para>
/// </summary>
public static class ElfParser
{
    private const uint ELF_MAGIC = 0x464C457F;   // "\x7FELF"
    private const uint PT_LOAD = 1;

    private const int Elf32HeaderSize = 52;
    private const int Elf32PhdrSize = 32;        // ELF32 program headers are exactly this; anything else is malformed

    private const ushort ET_EXEC = 2;
    private const ushort EM_386 = 3;

    /// <summary>
    /// A linked CXOS artifact has a handful of PT_LOADs - the kernel, the largest,
    /// has five. The cap exists so an absurd e_phnum is rejected on sight rather
    /// than driving a 65535-iteration loop over a file that cannot contain them.
    /// </summary>
    private const int MaxProgramHeaders = 128;

    /// <summary>
    /// Ceiling on total mapped memory. The largest real image is the kernel at a
    /// little over 1 MB; 512 MiB is far beyond anything CXOS will load and still
    /// well inside the 32-bit address space, so a header claiming gigabytes is
    /// refused before anyone tries to allocate it.
    /// </summary>
    private const ulong MaxAggregateMemorySize = 512UL * 1024 * 1024;

    public static ElfImage Parse(byte[] elfData)
    {
        if (elfData is null) throw new ArgumentNullException(nameof(elfData));
        ReadOnlySpan<byte> span = elfData;

        // ---- identification ----
        if (span.Length < Elf32HeaderSize)
            throw new InvalidDataException(
                $"truncated ELF: {span.Length} bytes, but the ELF32 header alone is {Elf32HeaderSize}.");

        if (MemoryPrimitives.ReadU32(span, 0) != ELF_MAGIC)
            throw new InvalidDataException("not an ELF file: missing the \\x7FELF magic.");

        if (span[4] != 1 || span[5] != 1)
            throw new NotSupportedException("only 32-bit little-endian ELF is supported (EI_CLASS/EI_DATA).");

        if (span[6] != 1)
            throw new InvalidDataException($"unsupported ELF ident version {span[6]}, expected 1.");

        ushort eType = MemoryPrimitives.ReadU16(span, 16);
        if (eType != ET_EXEC)
            throw new InvalidDataException(
                $"ELF type {eType} is not ET_EXEC. CXEX packages fully linked executables, " +
                "not relocatable objects or shared images.");

        ushort eMachine = MemoryPrimitives.ReadU16(span, 18);
        if (eMachine != EM_386)
            throw new InvalidDataException($"ELF machine {eMachine} is not EM_386 (3).");

        // ---- the program header table ----
        uint entryPoint = MemoryPrimitives.ReadU32(span, 24);
        uint phOff = MemoryPrimitives.ReadU32(span, 28);
        ushort phEntSize = MemoryPrimitives.ReadU16(span, 42);
        ushort phNum = MemoryPrimitives.ReadU16(span, 44);

        if (phNum == 0)
            throw new InvalidDataException("ELF declares no program headers, so there is nothing to load.");

        if (phNum > MaxProgramHeaders)
            throw new InvalidDataException(
                $"ELF declares {phNum} program headers, beyond the {MaxProgramHeaders} this accepts.");

        if (phEntSize != Elf32PhdrSize)
            throw new InvalidDataException(
                $"e_phentsize is {phEntSize}; an ELF32 program header is exactly {Elf32PhdrSize} bytes.");

        // 64-bit throughout: phOff and the product are both 32-bit quantities that
        // would wrap if added in 32 bits, which is how a table "inside" the file
        // can actually start past its end.
        ulong tableEnd = (ulong)phOff + (ulong)phNum * phEntSize;
        if (tableEnd > (ulong)span.Length)
            throw new InvalidDataException(
                $"the program header table runs past the end of the file: " +
                $"e_phoff {phOff} + {phNum} x {phEntSize} = {tableEnd}, file is {span.Length} bytes.");

        // ---- the segments ----
        var segments = new List<ElfSegment>();
        ulong totalMemory = 0;

        for (int i = 0; i < phNum; i++)
        {
            int ph = (int)(phOff + (ulong)i * phEntSize);   // bounded by tableEnd above

            if (MemoryPrimitives.ReadU32(span, ph) != PT_LOAD) continue;

            var seg = new ElfSegment
            {
                Offset = MemoryPrimitives.ReadU32(span, ph + 4),
                Vaddr = MemoryPrimitives.ReadU32(span, ph + 8),
                Paddr = MemoryPrimitives.ReadU32(span, ph + 12),
                FileSize = MemoryPrimitives.ReadU32(span, ph + 16),
                MemSize = MemoryPrimitives.ReadU32(span, ph + 20),
                Flags = MemoryPrimitives.ReadU32(span, ph + 24),
                Align = MemoryPrimitives.ReadU32(span, ph + 28),
            };

            // p_filesz > p_memsz would mean more bytes on disk than the segment
            // occupies in memory - the surplus has nowhere to go.
            if (seg.FileSize > seg.MemSize)
                throw new InvalidDataException(
                    $"segment {i}: p_filesz {seg.FileSize} exceeds p_memsz {seg.MemSize}.");

            // Added in 64 bits so the sum cannot wrap back inside the file.
            ulong fileEnd = (ulong)seg.Offset + seg.FileSize;
            if (fileEnd > (ulong)span.Length)
                throw new InvalidDataException(
                    $"segment {i}: p_offset {seg.Offset} + p_filesz {seg.FileSize} = {fileEnd}, " +
                    $"past the end of a {span.Length}-byte file.");

            if (seg.VirtualEnd > uint.MaxValue)
                throw new InvalidDataException(
                    $"segment {i}: p_vaddr 0x{seg.Vaddr:X8} + p_memsz {seg.MemSize} leaves the 32-bit address space.");

            if (seg.Align > 1)
            {
                if ((seg.Align & (seg.Align - 1)) != 0)
                    throw new InvalidDataException($"segment {i}: p_align {seg.Align} is not a power of two.");

                if ((seg.Vaddr & (seg.Align - 1)) != (seg.Offset & (seg.Align - 1)))
                    throw new InvalidDataException(
                        $"segment {i}: p_vaddr 0x{seg.Vaddr:X8} and p_offset {seg.Offset} disagree " +
                        $"modulo p_align {seg.Align}, so the segment cannot be mapped from the file as laid out.");
            }

            totalMemory += seg.MemSize;
            if (totalMemory > MaxAggregateMemorySize)
                throw new InvalidDataException(
                    $"segments total {totalMemory} bytes of memory, beyond the {MaxAggregateMemorySize} this accepts.");

            if (seg.FileSize > 0)
                seg.Data = span.Slice((int)seg.Offset, (int)seg.FileSize).ToArray();

            segments.Add(seg);
        }

        if (segments.Count == 0)
            throw new InvalidDataException("no PT_LOAD segments in the ELF: nothing to map.");

        // ---- whole-image invariants ----
        // Overlapping loadable segments would let one quietly overwrite another
        // after the loader had already validated both.
        for (int a = 0; a < segments.Count; a++)
        {
            for (int b = a + 1; b < segments.Count; b++)
            {
                ElfSegment x = segments[a], y = segments[b];
                if (x.MemSize == 0 || y.MemSize == 0) continue;

                if (x.Vaddr < y.VirtualEnd && y.Vaddr < x.VirtualEnd)
                    throw new InvalidDataException(
                        $"segments {a} and {b} overlap in memory: " +
                        $"[0x{x.Vaddr:X8},0x{x.VirtualEnd:X8}) and [0x{y.Vaddr:X8},0x{y.VirtualEnd:X8}).");
            }
        }

        // An entry point outside every executable segment is a program that cannot
        // start - better to say so here than to hand the kernel an image that
        // faults on its first instruction.
        bool entryIsMapped = false;
        foreach (ElfSegment seg in segments)
        {
            if (seg.IsExecutable && entryPoint >= seg.Vaddr && entryPoint < seg.VirtualEnd)
            {
                entryIsMapped = true;
                break;
            }
        }

        if (!entryIsMapped)
            throw new InvalidDataException(
                $"entry point 0x{entryPoint:X8} lies outside every executable PT_LOAD segment.");

        return new ElfImage(entryPoint, segments);
    }
}
