using System.Collections.Generic;
using CXEX.Disk.Models;

namespace CXEX.Disk.Parsers;

/// <summary>Parses a classic MBR partition table (4 primary entries @ 0x1BE).</summary>
public static class MbrParser
{
    public static DiskLayout Parse(byte[] sector0, long diskSize, int sectorSize = 512)
    {
        var parts = new List<PartitionEntry>();
        for (int i = 0; i < 4; i++)
        {
            int o = 0x1BE + i * 16;
            byte status = sector0[o];
            byte type   = sector0[o + 4];
            uint startLba = ReadLE32(sector0, o + 8);
            uint sectors  = ReadLE32(sector0, o + 12);
            if (type == 0x00 || sectors == 0) continue;

            parts.Add(new PartitionEntry
            {
                Index = i,
                Name = $"Partition {i + 1}",
                TypeName = MbrType(type),
                TypeId = $"0x{type:X2}",
                StartLba = startLba,
                SectorCount = sectors,
                StartOffset = (long)startLba * sectorSize,
                SizeBytes = (long)sectors * sectorSize,
                Bootable = status == 0x80,
            });
        }
        return new DiskLayout
        {
            TableType = PartitionTableType.MBR,
            SectorSize = sectorSize,
            DiskSizeBytes = diskSize,
            Partitions = parts,
        };
    }

    private static uint ReadLE32(byte[] b, int o) =>
        (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24);

    private static string MbrType(byte t) => t switch
    {
        0x00 => "Empty",
        0x07 => "NTFS / exFAT",
        0x0B or 0x0C => "FAT32",
        0x0E => "FAT16",
        0x83 => "Linux",
        0x82 => "Linux swap",
        0xEE => "GPT protective",
        0xEF => "EFI System",
        0xA5 => "FreeBSD",
        _ => $"Type 0x{t:X2}",
    };
}
