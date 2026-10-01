using System;
using System.IO;
using CXEX.Disk.Models;
using CXEX.Disk.Parsers;

namespace CXEX.Disk;

/// <summary>
/// Entry point: sniff a disk image's partition table and parse it. Reads only the
/// first sectors (plus the GPT entry array) - never loads the whole image.
/// Detection order: GPT ("EFI PART" @ LBA1) -> XBPT ("XBPT" @ LBA1) -> MBR (0x55AA @ 0x1FE).
/// </summary>
public static class DiskAnalyzer
{
    public static DiskLayout Analyze(string path, int sectorSize = 512)
    {
        var fi = new FileInfo(path);
        using var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        return Analyze(s, fi.Length, sectorSize);
    }

    public static DiskLayout Analyze(Stream s, long diskSize, int sectorSize = 512)
    {
        // read LBA0 + LBA1
        int headLen = sectorSize * 2;
        var head = new byte[headLen];
        s.Seek(0, SeekOrigin.Begin);
        int r = 0; while (r < headLen) { int n = s.Read(head, r, headLen - r); if (n == 0) break; r += n; }

        int lba1 = sectorSize;

        // GPT: "EFI PART"
        if (Match(head, lba1, "EFI PART"u8))
            return GptParser.Parse(s, diskSize, sectorSize);

        // XBPT: "XBPT"
        if (head.Length >= lba1 + 4 &&
            head[lba1] == XbptParser.Magic[0] && head[lba1 + 1] == XbptParser.Magic[1] &&
            head[lba1 + 2] == XbptParser.Magic[2] && head[lba1 + 3] == XbptParser.Magic[3])
        {
            var lba1Bytes = new byte[sectorSize];
            Array.Copy(head, lba1, lba1Bytes, 0, Math.Min(sectorSize, head.Length - lba1));
            return XbptParser.Parse(lba1Bytes, diskSize, sectorSize);
        }

        // MBR: boot signature 0x55AA at 0x1FE
        if (head.Length >= 512 && head[510] == 0x55 && head[511] == 0xAA)
            return MbrParser.Parse(head, diskSize, sectorSize);

        return new DiskLayout
        {
            TableType = PartitionTableType.Unknown,
            SectorSize = sectorSize,
            DiskSizeBytes = diskSize,
            Note = "No recognizable partition table (GPT/XBPT/MBR) at LBA0/LBA1.",
        };
    }

    private static bool Match(byte[] data, int offset, ReadOnlySpan<byte> sig)
    {
        if (data.Length < offset + sig.Length) return false;
        for (int i = 0; i < sig.Length; i++) if (data[offset + i] != sig[i]) return false;
        return true;
    }
}
