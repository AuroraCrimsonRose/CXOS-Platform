using System;
using System.IO;
using System.Text;
using CXEX.Build.Layout;
using CXEX.Core.Constants;
using CXEX.Core.Utilities;

namespace CXEX.Build.Emitters;

public static class CXEXWriter
{
    public static void WriteExecutable(string outPath, CxexMemoryLayout layout)
    {
        if (layout.Sections.Count == 0)
            throw new InvalidDataException("no sections: there is nothing to write.");

        if (layout.Sections.Count > ushort.MaxValue)
            throw new InvalidDataException(
                $"{layout.Sections.Count} sections exceeds the 16-bit section_count field.");

        // 1. Final file size: Header(56) + Sections(28 * count) + raw segment data.
        // Summed in 64 bits. As uint this wrapped, and a wrapped total allocates a
        // buffer smaller than the data about to be written into it - so the first
        // section with a real offset either throws from inside Span.Slice or lands
        // somewhere it does not belong.
        long totalSize = 56 + (long)layout.Sections.Count * 28;
        foreach (var sec in layout.Sections)
        {
            if (sec.FileSize > 0 && sec.Payload.Length != sec.FileSize)
                throw new InvalidDataException(
                    $"section '{sec.Name}' declares file_size {sec.FileSize} but carries {sec.Payload.Length} bytes.");

            // W^X. The loader refuses such a section and CXEXLayoutEngine refuses to
            // build one, but WriteExecutable takes a layout from any caller, so it
            // does not assume either ran first (security review §11).
            if ((sec.Flags & CXFlags.SEC_WRITE) != 0 && (sec.Flags & CXFlags.SEC_EXEC) != 0)
                throw new InvalidDataException(
                    $"section '{sec.Name}' is both writable and executable; W^X forbids it.");

            totalSize += sec.FileSize;
        }

        if (totalSize > int.MaxValue)
            throw new InvalidDataException($"the image would be {totalSize} bytes, which cannot be written as one array.");

        // Every section's bytes must lie in the image and after the section table.
        // This is also what keeps the signed range honest: the signature is appended
        // at the end, so data placed before it is data the signature covers.
        long tableEnd = 56 + (long)layout.Sections.Count * 28;
        foreach (var sec in layout.Sections)
        {
            if (sec.FileSize == 0) continue;

            long end = (long)sec.FileOffset + sec.FileSize;
            if (sec.FileOffset < tableEnd || end > totalSize)
                throw new InvalidDataException(
                    $"section '{sec.Name}': bytes [{sec.FileOffset},{end}) fall outside the image body " +
                    $"[{tableEnd},{totalSize}).");
        }

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

        // 4. Output to disk, atomically (security review §8).
        //
        // Written to a temporary file beside the target and then moved over it, so
        // the output is never observed half-written. The failure this prevents is
        // specific: a build interrupted during WriteAllBytes leaves a truncated
        // .xkex that still has a valid header and a plausible section table, and
        // the next step signs it. A move either happens or does not.
        string? dir = Path.GetDirectoryName(outPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        string temp = outPath + ".tmp" + Environment.ProcessId;
        try
        {
            File.WriteAllBytes(temp, fileData);
            File.Move(temp, outPath, overwrite: true);
        }
        catch
        {
            // Do not leave the scratch file behind for the next run to trip over.
            try { if (File.Exists(temp)) File.Delete(temp); } catch { /* best effort */ }
            throw;
        }
    }
}