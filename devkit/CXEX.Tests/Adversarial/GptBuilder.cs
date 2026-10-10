// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Disk.Parsers;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// Builds a well-formed GPT disk image in memory, so every adversarial case can
/// be a single mutation of it - the shape <see cref="Elf32Builder"/> and
/// <see cref="CxexBuilder"/> use, and for the same reason: a validator test
/// whose cases are each malformed in several ways cannot tell you which check
/// caught them.
/// </summary>
public sealed class GptBuilder
{
    public const int SectorSize = 512;
    public const int HeaderLba = 1;
    public const int EntryArrayLba = 2;
    public const int EntrySize = 128;

    public int TotalSectors { get; set; } = 256;
    public uint NumEntries { get; set; } = 4;
    public uint EntrySizeField { get; set; } = EntrySize;
    public ulong EntryArrayLbaField { get; set; } = EntryArrayLba;
    public uint HeaderSizeField { get; set; } = 92;
    public bool CorruptSignature { get; set; }
    public bool CorruptHeaderCrc { get; set; }
    public bool CorruptEntriesCrc { get; set; }
    /// <summary>Truncate the produced image to this many bytes (0 = don't).</summary>
    public int TruncateTo { get; set; }

    public sealed class Part
    {
        public string TypeGuid = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
        public ulong First = 34;
        public ulong Last = 100;
        public string Name = "data";
    }

    public List<Part> Partitions { get; } = new();

    public static GptBuilder Valid()
    {
        var b = new GptBuilder();
        b.Partitions.Add(new Part { First = 34, Last = 100, Name = "system" });
        b.Partitions.Add(new Part { First = 101, Last = 200, Name = "data" });
        return b;
    }

    public byte[] Build()
    {
        int total = TotalSectors * SectorSize;
        var img = new byte[total];

        // protective MBR boot signature, so MBR sniffing would also see
        // something plausible if GPT detection were skipped
        img[510] = 0x55; img[511] = 0xAA;

        // ---- entry array ----
        int arrayBytes = (int)(NumEntries * EntrySizeField);
        var arr = new byte[arrayBytes];
        for (int i = 0; i < Partitions.Count && i < NumEntries; i++)
        {
            int o = (int)(i * EntrySizeField);
            var p = Partitions[i];
            System.Guid.Parse(p.TypeGuid).ToByteArray().CopyTo(arr, o);
            System.Guid.NewGuid().ToByteArray().CopyTo(arr, o + 16);
            WriteLE64(arr, o + 32, p.First);
            WriteLE64(arr, o + 40, p.Last);
            var nm = System.Text.Encoding.Unicode.GetBytes(p.Name);
            Array.Copy(nm, 0, arr, o + 56, Math.Min(nm.Length, 72));
        }
        int arrayOff = (int)(EntryArrayLbaField * SectorSize);
        if (arrayOff >= 0 && arrayOff + arrayBytes <= total)
            Array.Copy(arr, 0, img, arrayOff, arrayBytes);

        uint entriesCrc = GptParser.Crc32(arr, 0, arrayBytes);
        if (CorruptEntriesCrc) entriesCrc ^= 0xFFFFFFFFu;

        // ---- header at LBA1 ----
        int h = HeaderLba * SectorSize;
        byte[] sig = { 0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54 };   // "EFI PART"
        sig.CopyTo(img, h);
        if (CorruptSignature) img[h + 3] = 0x58;

        WriteLE32(img, h + 8, 0x00010000);                 // revision 1.0
        WriteLE32(img, h + 12, HeaderSizeField);
        WriteLE32(img, h + 16, 0);                         // header CRC, filled below
        WriteLE64(img, h + 24, HeaderLba);                 // my LBA
        WriteLE64(img, h + 32, (ulong)(TotalSectors - 1)); // alternate LBA
        WriteLE64(img, h + 40, 34);                        // first usable
        WriteLE64(img, h + 48, (ulong)(TotalSectors - 34));// last usable
        System.Guid.NewGuid().ToByteArray().CopyTo(img, h + 56);
        WriteLE64(img, h + 72, EntryArrayLbaField);
        WriteLE32(img, h + 80, NumEntries);
        WriteLE32(img, h + 84, EntrySizeField);
        WriteLE32(img, h + 88, entriesCrc);

        uint hdrCrc = GptParser.Crc32(img, h, (int)Math.Min(HeaderSizeField, (uint)SectorSize));
        if (CorruptHeaderCrc) hdrCrc ^= 0xFFFFFFFFu;
        WriteLE32(img, h + 16, hdrCrc);

        if (TruncateTo > 0 && TruncateTo < total)
        {
            var cut = new byte[TruncateTo];
            Array.Copy(img, cut, TruncateTo);
            return cut;
        }
        return img;
    }

    public MemoryStream Stream() => new MemoryStream(Build(), writable: false);

    private static void WriteLE32(byte[] b, int o, uint v)
    { b[o] = (byte)v; b[o+1] = (byte)(v>>8); b[o+2] = (byte)(v>>16); b[o+3] = (byte)(v>>24); }
    private static void WriteLE64(byte[] b, int o, ulong v)
    { for (int i = 0; i < 8; i++) b[o+i] = (byte)(v >> (8*i)); }
}
