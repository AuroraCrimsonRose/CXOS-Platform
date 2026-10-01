using System;
using System.IO;
using System.Text;
using CXEX.Build.Layout;
using CXEX.Core.Utilities;

namespace CXEX.Build.Emitters;

public static class CXEXWriter
{
    public static void WriteExecutable(string outPath, CxexMemoryLayout layout)
    {
        // 1. Calculate final file size: Header(56) + Sections(28 * count) + raw segment data
        uint totalSize = 56 + (uint)layout.Sections.Count * 28;
        foreach (var sec in layout.Sections) totalSize += sec.FileSize;

        byte[] fileData = new byte[totalSize];
        Span<byte> span = fileData;

        // 2. Write the 56-byte CXEX Header
        MemoryPrimitives.WriteU32(span, 0, 0x58455843); // 'CXEX'
        MemoryPrimitives.WriteU16(span, 4, layout.TypeCode);
        MemoryPrimitives.WriteU16(span, 6, layout.FormatVersion);
        MemoryPrimitives.WriteU16(span, 8, layout.ArchTarget);
        MemoryPrimitives.WriteU16(span, 10, layout.AbiVersion);
        MemoryPrimitives.WriteU32(span, 12, layout.Flags);
        MemoryPrimitives.WriteU32(span, 16, layout.EntryPoint);
        MemoryPrimitives.WriteU32(span, 20, layout.LoadBase);
        MemoryPrimitives.WriteU32(span, 24, layout.ImageMin);
        MemoryPrimitives.WriteU32(span, 28, layout.ImageMax);
        MemoryPrimitives.WriteU16(span, 32, (ushort)layout.Sections.Count);
        MemoryPrimitives.WriteU16(span, 34, 56); // SectionOffset is always 56
        MemoryPrimitives.WriteU32(span, 36, 0); // RelocOffset
        MemoryPrimitives.WriteU32(span, 40, 0); // SignatureOffset
        MemoryPrimitives.WriteU32(span, 44, 0); // DependencyOffset

        // phys_base at offset 48. boot/cxexload.asm (CXH_PHYS_BASE equ 48) uses this to
        // compute the virt->phys delta when placing a higher-half kernel with paging off.
        MemoryPrimitives.WriteU32(span, 48, layout.PhysBase);

        // 3. Write Section Table and Segment Data
        int secOffset = 56;
        foreach (var sec in layout.Sections)
        {
            // Write 8-byte NUL-padded name. Names longer than 8 bytes are truncated,
            // matching mkcxes.py's "8s" pack. (The previous form threw on any name
            // over 8 chars: CopyTo requires the destination to be at least as long
            // as the source, and the destination was clamped to 8.)
            byte[] nameBytes = Encoding.ASCII.GetBytes(sec.Name);
            int nameLen = Math.Min(nameBytes.Length, 8);
            nameBytes.AsSpan(0, nameLen).CopyTo(span.Slice(secOffset, nameLen));
            // remaining bytes are already zero (fileData is zero-initialised)

            // Write 20 bytes of properties
            MemoryPrimitives.WriteU32(span, secOffset + 8, sec.FileOffset);
            MemoryPrimitives.WriteU32(span, secOffset + 12, sec.VirtualAddress);
            MemoryPrimitives.WriteU32(span, secOffset + 16, sec.FileSize);
            MemoryPrimitives.WriteU32(span, secOffset + 20, sec.MemSize);
            MemoryPrimitives.WriteU32(span, secOffset + 24, sec.Flags);

            // Copy raw segment data into place
            if (sec.FileSize > 0)
            {
                sec.Payload.CopyTo(span.Slice((int)sec.FileOffset, (int)sec.FileSize));
            }

            secOffset += 28;
        }

        // 4. Output to disk
        string? dir = Path.GetDirectoryName(outPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        File.WriteAllBytes(outPath, fileData);
    }
}