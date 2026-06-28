using System.Collections.Generic;
using CXEX.Disk.Models;
using CXEX.FileType.Parsers;        // CXOtherParsers
using CXEX.FileType.Structures;     // XBPTHeader, XBPTEntry

namespace CXEX.Disk.Parsers;

/// <summary>
/// XBPT (X Boot Partition Table) reader. Delegates the on-disk byte layout to
/// CXEX.FileType.CXOtherParsers so the format is defined in exactly one place.
/// Header @ LBA1, 32-byte entries from offset 32.
/// </summary>
public static class XbptParser
{
    /// <summary>"XBPT" as it appears on disk (little-endian of CXMagic.XBPT). Used by DiskAnalyzer to sniff LBA1.</summary>
    public static readonly byte[] Magic = { 0x58, 0x42, 0x50, 0x54 };

    // CXFlags PART_CX* values (CXEX.Core.Constants.CXFlags)
    private const byte PART_CXBOOT = 0xCB, PART_CXSTAGE = 0xCA, PART_CXFS = 0xC5;

    public static DiskLayout Parse(byte[] lba1, long diskSize, int sectorSize = 512)
    {
        XBPTHeader hdr = CXOtherParsers.ParseXbptHeader(lba1);
        int entrySize = hdr.EntrySize == 0 ? 32 : hdr.EntrySize;

        var parts = new List<PartitionEntry>();
        int o = 32;
        for (int i = 0; i < hdr.EntryCount && o + entrySize <= lba1.Length; i++, o += entrySize)
        {
            XBPTEntry e = CXOtherParsers.ParseXbptEntry(lba1, o);
            parts.Add(new PartitionEntry
            {
                Index = i,
                Name = string.IsNullOrWhiteSpace(e.Name) ? $"Partition {i + 1}" : e.Name,
                TypeName = TypeName(e.PartitionType, e.Name),
                TypeId = $"0x{e.PartitionType:X2}",
                StartLba = (long)e.StartLba,
                SectorCount = (long)e.SectorCount,
                StartOffset = (long)e.StartLba * sectorSize,
                SizeBytes = (long)e.SectorCount * sectorSize,
                Bootable = (e.Flags & 0x01) != 0,   // PART_FLAG_BOOTABLE
            });
        }

        return new DiskLayout
        {
            TableType = PartitionTableType.XBPT,
            SectorSize = sectorSize,
            DiskSizeBytes = diskSize > 0 ? diskSize : (long)hdr.TotalSectors * sectorSize,
            Partitions = parts,
        };
    }

    private static string TypeName(byte t, string label) => t switch
    {
        PART_CXBOOT => "CX Boot",
        PART_CXSTAGE => "CX Stage",
        PART_CXFS => "CXFS System",
        _ => string.IsNullOrWhiteSpace(label) ? $"CX type 0x{t:X2}" : label,
    };
}