// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;
using CXEX.Core.Constants;
using CXEX.Disk.Parsers;

namespace CXEX.Disk;

/// <summary>What <see cref="CrashLog.Read"/> found in a disk's crash area.</summary>
public enum CrashAreaState
{
    /// <summary>A whole record: <see cref="CrashAreaReading.Text"/> holds it.</summary>
    Record,
    /// <summary>User processes that faulted after the self-tests, and the system
    /// log (no panic): <see cref="CrashAreaReading.Text"/> holds it.</summary>
    FaultRecord,
    /// <summary>A test request, armed by <c>cxk verify-crashlog</c> and not yet taken.</summary>
    Request,
    /// <summary>The area is there and holds nothing.</summary>
    Empty,
    /// <summary>The disk has no XBPT table, or the table no CRASH partition.</summary>
    NoArea,
    /// <summary>Something is there but it is not a record this reader accepts.</summary>
    Invalid,
}

public sealed record CrashAreaReading(CrashAreaState State, string Detail, string Text = "");

/// <summary>
/// The XCRL crash record in a disk's CRASH partition: what a kernel panic
/// leaves for the next boot (kernel/diag/crashrec.h is the other half, and the
/// layout is documented there). Read here so a crash log can be pulled off a
/// disk image without booting it, and armed here so a test can ask a boot to
/// panic on purpose.
/// </summary>
public static class CrashLog
{
    public const uint Magic = 0x4C524358;   // "XCRL"

    /// <summary>Declared in versions.json as <c>xcrl</c>, with XCRL_VERSION in kernel/diag/crashrec.h.</summary>
    public const int XcrlVersion = 1;

    public const ushort KindCrash = 1;
    public const ushort KindRequest = 2;
    public const ushort KindFault = 3;
    public const int Sector = 512;
    public const int TextMax = 32 * Sector;
    public const int Sectors = 1 + TextMax / Sector;

    /// <summary>What a save writes into the record when it had to reset a USB
    /// disk a panic left mid-transfer (CRASHLOG_RESET_LINE, kernel/diag/crashlog.h).</summary>
    public const string UsbResetLine = "crash log: the disk was mid-transfer; reset before this save";

    /// <summary>CRC-32 (IEEE, reflected, as zlib), chained like the kernel's xcrl_crc32.</summary>
    public static uint Crc32(uint crc, ReadOnlySpan<byte> data)
    {
        crc = ~crc;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int i = 0; i < 8; i++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    /// <summary>The 512-byte header for <paramref name="text"/>.</summary>
    public static byte[] Seal(ushort kind, ReadOnlySpan<byte> text)
    {
        if (text.Length > TextMax) throw new ArgumentException($"a crash record holds at most {TextMax} bytes");
        var h = new byte[Sector];
        BitConverter.TryWriteBytes(h.AsSpan(0), Magic);
        BitConverter.TryWriteBytes(h.AsSpan(4), (ushort)XcrlVersion);
        BitConverter.TryWriteBytes(h.AsSpan(6), kind);
        BitConverter.TryWriteBytes(h.AsSpan(8), (uint)text.Length);
        uint crc = Crc32(Crc32(0, h.AsSpan(0, 12)), text);
        BitConverter.TryWriteBytes(h.AsSpan(12), crc);
        return h;
    }

    /// <summary>The CRASH partition's first sector and length, or null.</summary>
    public static (long Lba, long Sectors)? FindArea(Stream disk)
    {
        var lba1 = new byte[Sector];
        disk.Seek(Sector, SeekOrigin.Begin);
        if (disk.ReadAtLeast(lba1, Sector, throwOnEndOfStream: false) < Sector) return null;
        if (!lba1.AsSpan(0, 4).SequenceEqual(XbptParser.Magic)) return null;
        var layout = XbptParser.Parse(lba1, disk.Length);
        var p = layout.Partitions.FirstOrDefault(e => e.TypeId == $"0x{CXFlags.PART_CXCRASH:X2}");
        return p is null ? null : (p.StartLba, p.SectorCount);
    }

    public static CrashAreaReading Read(Stream disk)
    {
        var area = FindArea(disk);
        if (area is null) return new(CrashAreaState.NoArea, "no XBPT table with a CRASH partition");
        if (area.Value.Sectors < Sectors) return new(CrashAreaState.Invalid, $"the CRASH partition is {area.Value.Sectors} sectors, short of {Sectors}");

        var h = new byte[Sector];
        disk.Seek(area.Value.Lba * Sector, SeekOrigin.Begin);
        disk.ReadExactly(h);

        if (BitConverter.ToUInt32(h, 0) != Magic) return new(CrashAreaState.Empty, "the crash area is empty");
        ushort ver = BitConverter.ToUInt16(h, 4), kind = BitConverter.ToUInt16(h, 6);
        uint len = BitConverter.ToUInt32(h, 8);
        if (ver != XcrlVersion) return new(CrashAreaState.Invalid, $"record version {ver}; this cxk reads {XcrlVersion}");
        if (kind != KindCrash && kind != KindRequest && kind != KindFault) return new(CrashAreaState.Invalid, $"record of unknown kind {kind}");
        if (len > TextMax || (kind == KindRequest && len != 0)) return new(CrashAreaState.Invalid, $"record length {len} out of range");

        var text = new byte[len];
        if (len > 0)
        {
            disk.Seek((area.Value.Lba + 1) * Sector, SeekOrigin.Begin);
            disk.ReadExactly(text);
        }
        uint crc = Crc32(Crc32(0, h.AsSpan(0, 12)), text);
        if (crc != BitConverter.ToUInt32(h, 12)) return new(CrashAreaState.Invalid, "record torn: the CRC does not match");

        return kind switch
        {
            KindRequest => new(CrashAreaState.Request, "a crash-log test request, not yet taken"),
            KindFault => new(CrashAreaState.FaultRecord, $"user-fault record (no panic), {len} bytes", Encoding.ASCII.GetString(text)),
            _ => new(CrashAreaState.Record, $"crash record, {len} bytes", Encoding.ASCII.GetString(text)),
        };
    }

    /// <summary>Ask the next boot of this disk to panic on purpose (the kernel clears it first).</summary>
    public static void Arm(Stream disk)
    {
        var area = FindArea(disk) ?? throw new InvalidOperationException("the disk has no CRASH partition");
        disk.Seek(area.Lba * Sector, SeekOrigin.Begin);
        disk.Write(Seal(KindRequest, ReadOnlySpan<byte>.Empty));
        disk.Flush();
    }
}
