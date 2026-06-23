using System;
using System.Collections.Generic;
using CXEX.Build.Layout;
using CXEX.Build.Parsers;
using CXEX.Core.Constants;

namespace CXEX.Build.Engines;

public static class CXEXLayoutEngine
{
    public static CxexMemoryLayout CreateLayout(uint entryPoint, List<ElfSegment> loadSegments, ushort typeCode, ushort abiVersion = 1)
    {
        var layout = new CxexMemoryLayout
        {
            TypeCode = typeCode,
            AbiVersion = abiVersion,
            EntryPoint = entryPoint,
            ImageMin = uint.MaxValue,
            ImageMax = 0,
            PhysBase = uint.MaxValue,
            Flags = CXFlags.FLAG_EXECUTABLE | CXFlags.FLAG_REQUIRE_ABI_MATCH | CXFlags.FLAG_REQUIRE_ARCH_MATCH
        };

        if (typeCode == CXFlags.TYPE_KERNEL)
            layout.Flags |= CXFlags.FLAG_KERNEL_PRIV;

        // 1. Calculate boundaries
        foreach (var seg in loadSegments)
        {
            if (seg.Vaddr < layout.ImageMin) layout.ImageMin = seg.Vaddr;
            if (seg.Vaddr + seg.MemSize > layout.ImageMax) layout.ImageMax = seg.Vaddr + seg.MemSize;
            if (seg.Paddr < layout.PhysBase) layout.PhysBase = seg.Paddr;
        }

        layout.LoadBase = layout.ImageMin;

        // 2. Map file offsets (Header = 56 bytes, Section Entries = 28 bytes each)
        uint fileCursor = 56 + (uint)(loadSegments.Count * 28);

        foreach (var seg in loadSegments)
        {
            uint cxFlags = 0;
            if ((seg.Flags & 0x4) != 0) cxFlags |= CXFlags.SEC_READ;
            if ((seg.Flags & 0x2) != 0) cxFlags |= CXFlags.SEC_WRITE;
            if ((seg.Flags & 0x1) != 0) cxFlags |= CXFlags.SEC_EXEC;

            // Infer name based on execution flags
            string name = (seg.Flags & 0x1) != 0 ? ".text" : ((seg.Flags & 0x2) != 0 ? ".data" : ".rodata");

            uint fileOffset = 0;
            if (seg.FileSize == 0)
            {
                cxFlags |= CXFlags.SEC_NOBITS;
            }
            else
            {
                fileOffset = fileCursor;
                fileCursor += seg.FileSize;
            }

            layout.Sections.Add(new SectionLayout
            {
                Name = name,
                VirtualAddress = seg.Vaddr,
                FileOffset = fileOffset,
                FileSize = seg.FileSize,
                MemSize = seg.MemSize,
                Flags = cxFlags,
                Payload = seg.Data
            });
        }

        return layout;
    }
}