using System;
using System.Collections.Generic;
using System.Text;
using CXEX.Disk.Models;

namespace CXEX.Disk.Parsers;

/// <summary>Parses a GPT header (LBA1) + its partition entry array.</summary>
public static class GptParser
{
    public static DiskLayout Parse(System.IO.Stream s, long diskSize, int sectorSize = 512)
    {
        var hdr = ReadAt(s, sectorSize, 92);                 // GPT header @ LBA1
        long entryArrayLba = (long)ReadLE64(hdr, 72);
        uint numEntries    = ReadLE32(hdr, 80);
        uint entrySize     = ReadLE32(hdr, 84);
        string diskGuid    = Guid(hdr, 56);

        var parts = new List<PartitionEntry>();
        if (entrySize >= 128 && numEntries is > 0 and <= 256)
        {
            var arr = ReadAt(s, entryArrayLba * sectorSize, (int)(numEntries * entrySize));
            int idx = 0;
            for (uint i = 0; i < numEntries; i++)
            {
                int o = (int)(i * entrySize);
                if (IsZero(arr, o, 16)) continue;            // empty type GUID -> unused
                long first = (long)ReadLE64(arr, o + 32);
                long last  = (long)ReadLE64(arr, o + 40);
                string name = Encoding.Unicode.GetString(arr, o + 56, 72).TrimEnd('\0');
                long sectors = last - first + 1;
                parts.Add(new PartitionEntry
                {
                    Index = idx++,
                    Name = string.IsNullOrWhiteSpace(name) ? $"Partition {idx}" : name,
                    TypeName = GptType(Guid(arr, o)),
                    TypeId = Guid(arr, o),
                    Guid = Guid(arr, o + 16),
                    StartLba = first,
                    SectorCount = sectors,
                    StartOffset = first * sectorSize,
                    SizeBytes = sectors * sectorSize,
                });
            }
        }
        return new DiskLayout
        {
            TableType = PartitionTableType.GPT,
            SectorSize = sectorSize,
            DiskSizeBytes = diskSize,
            DiskGuid = diskGuid,
            Partitions = parts,
        };
    }

    private static byte[] ReadAt(System.IO.Stream s, long off, int len)
    {
        s.Seek(off, System.IO.SeekOrigin.Begin);
        var b = new byte[len]; int r = 0;
        while (r < len) { int n = s.Read(b, r, len - r); if (n == 0) break; r += n; }
        return b;
    }
    private static uint ReadLE32(byte[] b, int o) => (uint)(b[o] | b[o+1]<<8 | b[o+2]<<16 | b[o+3]<<24);
    private static ulong ReadLE64(byte[] b, int o)
    { ulong v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | b[o + i]; return v; }
    private static bool IsZero(byte[] b, int o, int n) { for (int i=0;i<n;i++) if (b[o+i]!=0) return false; return true; }
    private static string Guid(byte[] b, int o) => new Guid(new ReadOnlySpan<byte>(b, o, 16)).ToString().ToUpperInvariant();

    private static string GptType(string guid) => guid switch
    {
        "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" => "EFI System",
        "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" => "Microsoft Basic Data",
        "0FC63DAF-8483-4772-8E79-3D69D8477DE4" => "Linux filesystem",
        "21686148-6449-6E6F-744E-656564454649" => "BIOS boot",
        "00000000-0000-0000-0000-000000000000" => "Unused",
        _ => "Unknown",
    };
}
