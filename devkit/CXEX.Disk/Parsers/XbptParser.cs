using System.Collections.Generic;
using System.Text;
using CXEX.Disk.Models;

namespace CXEX.Disk.Parsers;

/// <summary>
/// XBPT (X Boot Partition Table) reader. Matches CXEX.Build.XBPTImageWriter.
/// Header @ LBA1: magic "XBPT" (u32), version (u16@4), count (u16@6),
/// entrySize (u16@8, =32), totalSectors (u64@12). Entries from @32, 32 bytes:
/// StartLba u64, SectorCount u64, TypeCode u8@16, Flags u8@17, Label ASCII@20[12].
/// </summary>
public static class XbptParser
{
    public static readonly byte[] Magic = { 0x58, 0x42, 0x50, 0x54 }; // "XBPT"

    public static DiskLayout Parse(byte[] lba1, long diskSize, int sectorSize = 512)
    {
        ushort count = ReadU16(lba1, 6);
        ushort entrySize = ReadU16(lba1, 8);
        if (entrySize == 0) entrySize = 32;
        ulong totalSectors = ReadU64(lba1, 12);

        var parts = new List<PartitionEntry>();
        int o = 32;
        for (int i = 0; i < count && o + entrySize <= lba1.Length; i++, o += entrySize)
        {
            ulong startLba = ReadU64(lba1, o);
            ulong sectors = ReadU64(lba1, o + 8);
            byte typeCode = lba1[o + 16];
            byte flags = lba1[o + 17];
            string label = ReadAscii(lba1, o + 20, 12);

            parts.Add(new PartitionEntry
            {
                Index = i,
                Name = string.IsNullOrWhiteSpace(label) ? $"Partition {i + 1}" : label,
                TypeName = TypeName(typeCode, label),
                TypeId = $"0x{typeCode:X2}",
                StartLba = (long)startLba,
                SectorCount = (long)sectors,
                StartOffset = (long)startLba * sectorSize,
                SizeBytes = (long)sectors * sectorSize,
                Bootable = (flags & 0x01) != 0,   // PART_FLAG_BOOTABLE
            });
        }

        return new DiskLayout
        {
            TableType = PartitionTableType.XBPT,
            SectorSize = sectorSize,
            DiskSizeBytes = diskSize > 0 ? diskSize : (long)totalSectors * sectorSize,
            Partitions = parts,
        };
    }

    // CXFlags PART_CX* byte values aren't known here; fall back to the label.
    private static string TypeName(byte t, string label) => label.ToUpperInvariant() switch
    {
        "BOOT" => "CX Boot",
        "STAGE" => "CX Stage",
        "SYSTEM" => "CXFS System",
        _ => $"CX type 0x{t:X2}",
    };

    private static ushort ReadU16(byte[] b, int o) => (ushort)(b[o] | b[o + 1] << 8);
    private static ulong ReadU64(byte[] b, int o)
    { ulong v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | b[o + i]; return v; }
    private static string ReadAscii(byte[] b, int o, int max)
    {
        int n = 0; while (n < max && o + n < b.Length && b[o + n] != 0) n++;
        return Encoding.ASCII.GetString(b, o, n);
    }
}