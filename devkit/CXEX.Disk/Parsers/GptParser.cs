// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Text;
using CXEX.Disk.Models;

namespace CXEX.Disk.Parsers;

/// <summary>
/// Parses a GPT header (LBA1) + its partition entry array.
///
/// <para>Hardened 2026-10-09 for the platform security review §2. A GPT is
/// untrusted input - the whole point of inspecting a disk image is that you do
/// not know what is in it - and this trusted it in five ways: it never checked
/// the signature or either CRC, it computed a seek offset and an allocation
/// size from unbounded header fields, and <c>ReadAt</c> reported a short read
/// as success by returning a zero-filled buffer, so a truncated image parsed
/// as partitions made of zeros rather than failing.</para>
///
/// <para>The sharpest one was the allocation: <c>entrySize</c> had no upper
/// bound, so <c>numEntries * entrySize</c> wrapped as <c>uint</c> BEFORE the
/// cast to <c>int</c> ever happened - 256 x 0x01000000 gives 0, a zero-length
/// buffer the loop then indexed.</para>
/// </summary>
public static class GptParser
{
    /// <summary>"EFI PART", the GPT header signature.</summary>
    private static readonly byte[] Signature = { 0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54 };

    /// <summary>
    /// A real GPT entry is 128 bytes; the spec allows larger, a multiple of 8.
    /// Capped because the size is only used to stride and to allocate, and an
    /// unbounded value does both wrongly. 4 KiB is far past anything real.
    /// </summary>
    private const uint MaxEntrySize = 4096;
    private const uint MinEntrySize = 128;
    private const uint MaxEntries = 256;

    public static DiskLayout Parse(System.IO.Stream s, long diskSize, int sectorSize = 512)
    {
        if (sectorSize <= 0 || (sectorSize & (sectorSize - 1)) != 0)
            throw new InvalidDataException($"sector size {sectorSize} is not a power of two.");

        // The stream length is the authority on what can be read, not the
        // caller's diskSize - they disagree for a truncated file, and the
        // smaller of the two is the only safe bound.
        long limit = s.CanSeek ? Math.Min(diskSize, s.Length) : diskSize;

        var hdr = ReadExact(s, (long)sectorSize, 92, "GPT header");

        // The signature IS checked by DiskAnalyzer before it dispatches here.
        // Checked again, deliberately, for the reason CXEXWriter refuses W+X
        // even though the layout engine already did (security review §11):
        // this method is public, so a direct caller gets no guarantee from a
        // check that lives in the caller it did not use.
        for (int i = 0; i < Signature.Length; i++)
        {
            if (hdr[i] != Signature[i])
                throw new InvalidDataException("not a GPT header: signature is not \"EFI PART\".");
        }

        uint headerSize = ReadLE32(hdr, 12);
        if (headerSize < 92 || headerSize > (uint)sectorSize)
            throw new InvalidDataException($"GPT header_size {headerSize} is outside 92..{sectorSize}.");

        // The header CRC is computed with its own field zeroed.
        uint headerCrcStated = ReadLE32(hdr, 16);
        var hdrForCrc = ReadExact(s, (long)sectorSize, (int)headerSize, "GPT header");
        hdrForCrc[16] = 0; hdrForCrc[17] = 0; hdrForCrc[18] = 0; hdrForCrc[19] = 0;
        uint headerCrcActual = Crc32(hdrForCrc, 0, (int)headerSize);
        if (headerCrcStated != headerCrcActual)
            throw new InvalidDataException(
                $"GPT header CRC mismatch: stated {headerCrcStated:X8}, computed {headerCrcActual:X8}.");

        ulong entryArrayLba = ReadLE64(hdr, 72);
        uint numEntries     = ReadLE32(hdr, 80);
        uint entrySize      = ReadLE32(hdr, 84);
        uint entriesCrc     = ReadLE32(hdr, 88);
        string diskGuid     = Guid(hdr, 56);

        if (entrySize < MinEntrySize || entrySize > MaxEntrySize || (entrySize % 8) != 0)
            throw new InvalidDataException(
                $"GPT entry size {entrySize} is outside {MinEntrySize}..{MaxEntrySize} or not a multiple of 8.");
        if (numEntries == 0 || numEntries > MaxEntries)
            throw new InvalidDataException($"GPT declares {numEntries} entries, beyond the {MaxEntries} this accepts.");

        // 64-bit throughout, and range-checked before either value is used.
        // entryArrayLba * sectorSize overflowed, and the product below wrapped
        // as uint before the cast to int.
        ulong arrayOffset = entryArrayLba * (ulong)sectorSize;
        ulong arrayBytes  = (ulong)numEntries * entrySize;
        if (entryArrayLba == 0)
            throw new InvalidDataException("GPT entry array starts at LBA 0, which is the protective MBR.");
        if (arrayOffset > (ulong)limit || arrayBytes > (ulong)limit - arrayOffset)
            throw new InvalidDataException(
                $"GPT entry array ({arrayBytes} bytes at offset {arrayOffset}) does not fit the {limit}-byte image.");

        var arr = ReadExact(s, (long)arrayOffset, (int)arrayBytes, "GPT entry array");

        uint entriesCrcActual = Crc32(arr, 0, (int)arrayBytes);
        if (entriesCrc != entriesCrcActual)
            throw new InvalidDataException(
                $"GPT entry array CRC mismatch: stated {entriesCrc:X8}, computed {entriesCrcActual:X8}.");

        long sectorCountTotal = limit / sectorSize;
        var parts = new List<PartitionEntry>();
        int idx = 0;

        for (uint i = 0; i < numEntries; i++)
        {
            int o = (int)(i * entrySize);
            if (IsZero(arr, o, 16)) continue;            // empty type GUID -> unused

            ulong first = ReadLE64(arr, o + 32);
            ulong last  = ReadLE64(arr, o + 40);

            // A partition that runs backwards, starts past the end, or ends
            // past the end is not a partition. Previously `last - first + 1`
            // could be negative or absurd and `first * sectorSize` could
            // overflow, so StartOffset and SizeBytes came out as nonsense.
            if (last < first) continue;
            if (first >= (ulong)sectorCountTotal || last >= (ulong)sectorCountTotal) continue;

            ulong sectors = last - first + 1;
            string name = Encoding.Unicode.GetString(arr, o + 56, 72).TrimEnd('\0');

            parts.Add(new PartitionEntry
            {
                Index = idx++,
                Name = string.IsNullOrWhiteSpace(name) ? $"Partition {idx}" : name,
                TypeName = GptType(Guid(arr, o)),
                TypeId = Guid(arr, o),
                Guid = Guid(arr, o + 16),
                StartLba = (long)first,
                SectorCount = (long)sectors,
                StartOffset = (long)(first * (ulong)sectorSize),
                SizeBytes = (long)(sectors * (ulong)sectorSize),
            });
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

    /// <summary>
    /// Reads exactly <paramref name="len"/> bytes, or throws.
    ///
    /// <para>This replaces a <c>ReadAt</c> that broke out of its read loop on
    /// a short read and returned the buffer anyway - zero-filled for the
    /// remainder. A truncated image therefore parsed cleanly into partitions
    /// made of zeros, which is a silent wrong answer and worse than the
    /// exception a caller expects.</para>
    /// </summary>
    private static byte[] ReadExact(System.IO.Stream s, long off, int len, string what)
    {
        if (len <= 0) throw new InvalidDataException($"{what}: refusing a {len}-byte read.");
        if (off < 0) throw new InvalidDataException($"{what}: offset {off} is negative.");

        s.Seek(off, System.IO.SeekOrigin.Begin);
        var b = new byte[len];
        int r = 0;
        while (r < len)
        {
            int n = s.Read(b, r, len - r);
            if (n == 0)
                throw new InvalidDataException(
                    $"{what}: image ends after {r} of {len} bytes at offset {off}.");
            r += n;
        }
        return b;
    }

    /// <summary>
    /// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) - what UEFI
    /// specifies for both GPT checksums. Computed rather than taken from a
    /// library so CXEX.Disk keeps its no-dependency shape.
    /// </summary>
    public static uint Crc32(byte[] data, int offset, int length)
    {
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < length; i++)
        {
            crc ^= data[offset + i];
            for (int bit = 0; bit < 8; bit++)
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return ~crc;
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
