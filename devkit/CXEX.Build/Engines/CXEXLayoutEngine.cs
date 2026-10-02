using System;
using System.Collections.Generic;
using System.IO;
using CXEX.Build.Layout;
using CXEX.Build.Parsers;
using CXEX.Core.Constants;

namespace CXEX.Build.Engines;

public static class CXEXLayoutEngine
{
    /// <summary>
    /// The ABI contract images are stamped with. It was 1 while the live contract
    /// (docs/kernel/CX_ABI.md, abi/cxk_abi.h CXK_ABI_VERSION) was 2 - and every
    /// image sets FLAG_REQUIRE_ABI_MATCH, asking to be refused on exactly that
    /// mismatch. It survived only because the kernel's cxex_check_compat has no
    /// caller, so nothing ever compared the two. Declared in versions.json as
    /// `cxex-abi` and checked against both sides.
    /// </summary>
    public const ushort AbiVersion = 2;

    public static CxexMemoryLayout CreateLayout(uint entryPoint, IReadOnlyList<ElfSegment> loadSegments, ushort typeCode, ushort abiVersion = AbiVersion)
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

        if (loadSegments.Count == 0)
            throw new InvalidDataException("no loadable segments: there is nothing to package.");

        // The section count is a 16-bit field in the header. Truncating it here
        // would emit an image whose table says one thing and whose bytes say
        // another, which is worse than refusing to emit anything.
        if (loadSegments.Count > ushort.MaxValue)
            throw new InvalidDataException(
                $"{loadSegments.Count} sections exceeds the 16-bit section_count field.");

        // 1. Calculate boundaries, in 64 bits so the sum cannot wrap back under
        // ImageMax and understate the image.
        long imageMin = uint.MaxValue, imageMax = 0, physBase = uint.MaxValue;
        foreach (var seg in loadSegments)
        {
            long end = (long)seg.Vaddr + seg.MemSize;
            if (end > uint.MaxValue)
                throw new InvalidDataException(
                    $"section '{NameFor(seg.Flags)}' at 0x{seg.Vaddr:X8} + {seg.MemSize} bytes leaves the 32-bit address space.");

            if (seg.Vaddr < imageMin) imageMin = seg.Vaddr;
            if (end > imageMax) imageMax = end;
            if (seg.Paddr < physBase) physBase = seg.Paddr;

            // W^X, the writer's half of the rule the loader enforces: a section
            // that is both writable and executable is refused at the point it
            // would be created, not left for the kernel to discover.
            if ((seg.Flags & 0x2) != 0 && (seg.Flags & 0x1) != 0)
                throw new InvalidDataException(
                    $"segment at 0x{seg.Vaddr:X8} is both writable and executable; W^X forbids it.");

            if (seg.Data.Length != seg.FileSize)
                throw new InvalidDataException(
                    $"segment at 0x{seg.Vaddr:X8} carries {seg.Data.Length} bytes but declares file_size {seg.FileSize}.");
        }

        layout.ImageMin = (uint)imageMin;
        layout.ImageMax = (uint)imageMax;
        layout.PhysBase = (uint)physBase;
        layout.LoadBase = layout.ImageMin;

        // 2. Map file offsets (Header = 56 bytes, Section Entries = 28 bytes each).
        // Accumulated as long: a 32-bit cursor can wrap past the end of the image
        // and start handing out offsets that point back into the header.
        long cursor = 56 + (long)loadSegments.Count * 28;

        foreach (var seg in loadSegments)
        {
            uint cxFlags = 0;
            if ((seg.Flags & 0x4) != 0) cxFlags |= CXFlags.SEC_READ;
            if ((seg.Flags & 0x2) != 0) cxFlags |= CXFlags.SEC_WRITE;
            if ((seg.Flags & 0x1) != 0) cxFlags |= CXFlags.SEC_EXEC;

            string name = NameFor(seg.Flags);

            uint fileOffset = 0;
            if (seg.FileSize == 0)
            {
                cxFlags |= CXFlags.SEC_NOBITS;
            }
            else
            {
                fileOffset = (uint)cursor;
                cursor += seg.FileSize;

                // file_offset is a 32-bit field, so an image whose data runs past
                // 4 GB cannot be described at all - and the next section's offset
                // would silently wrap into the header.
                if (cursor > uint.MaxValue)
                    throw new InvalidDataException(
                        $"section data passes 4 GB at '{name}'; the image cannot be addressed by a 32-bit file_offset.");
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

    /// <summary>
    /// Names a section from its ELF protection bits. Executable wins over
    /// writable, so a segment is ".text", ".data" or ".rodata" in that order.
    /// </summary>
    private static string NameFor(uint elfFlags) =>
        (elfFlags & 0x1) != 0 ? ".text" : ((elfFlags & 0x2) != 0 ? ".data" : ".rodata");
}