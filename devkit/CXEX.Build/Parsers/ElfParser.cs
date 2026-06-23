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
    public uint Flags { get; set; }
    public byte[] Data { get; set; } = Array.Empty<byte>();
}

public static class ElfParser
{
    private const uint ELF_MAGIC = 0x464C457F; // "\x7FELF"
    private const uint PT_LOAD = 1;

    public static (uint EntryPoint, List<ElfSegment> Segments) Parse(byte[] elfData)
    {
        ReadOnlySpan<byte> span = elfData;

        if (span.Length < 52 || MemoryPrimitives.ReadU32(span, 0) != ELF_MAGIC)
            throw new InvalidDataException("Invalid ELF32 magic signature.");

        if (span[4] != 1 || span[5] != 1)
            throw new NotSupportedException("Only 32-bit Little-Endian ELF targets are supported.");

        uint entryPoint = MemoryPrimitives.ReadU32(span, 24);
        uint phOff = MemoryPrimitives.ReadU32(span, 28);
        ushort phEntSize = MemoryPrimitives.ReadU16(span, 42);
        ushort phNum = MemoryPrimitives.ReadU16(span, 44);

        var segments = new List<ElfSegment>();

        for (int i = 0; i < phNum; i++)
        {
            int pHeaderOffset = (int)(phOff + (i * phEntSize));
            uint pType = MemoryPrimitives.ReadU32(span, pHeaderOffset);

            if (pType == PT_LOAD)
            {
                var seg = new ElfSegment
                {
                    Offset = MemoryPrimitives.ReadU32(span, pHeaderOffset + 4),
                    Vaddr = MemoryPrimitives.ReadU32(span, pHeaderOffset + 8),
                    Paddr = MemoryPrimitives.ReadU32(span, pHeaderOffset + 12),
                    FileSize = MemoryPrimitives.ReadU32(span, pHeaderOffset + 16),
                    MemSize = MemoryPrimitives.ReadU32(span, pHeaderOffset + 20),
                    Flags = MemoryPrimitives.ReadU32(span, pHeaderOffset + 24)
                };

                if (seg.FileSize > 0)
                {
                    seg.Data = span.Slice((int)seg.Offset, (int)seg.FileSize).ToArray();
                }

                segments.Add(seg);
            }
        }

        if (segments.Count == 0)
            throw new InvalidDataException("No PT_LOAD segments found in ELF. Nothing to map.");

        return (entryPoint, segments);
    }
}