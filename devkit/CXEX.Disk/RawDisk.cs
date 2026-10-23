// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace CXEX.Disk;

/// <summary>One physical disk as <c>cxk rig disks</c> lists it.</summary>
public sealed record PhysicalDiskInfo(int Number, string Path, long Bytes, string Bus, bool Removable, string Model);

/// <summary>
/// Whole disks, read-only: the rig's USB stick read directly instead of an
/// image of it. A disk is named by a device path (<c>\\.\PhysicalDrive5</c>,
/// <c>/dev/sdb</c>) or, on Windows, by its number as <c>disk5</c>, which
/// survives shells that eat backslashes.
///
/// <para>Nothing here writes. The handle is opened for reading only, so a
/// mistake in a caller cannot reach the disk, which matters when the disk
/// next to the stick is the machine's own.</para>
/// </summary>
public static class RawDisk
{
    public const int Sector = 512;

    /// <summary>The device path <paramref name="arg"/> names, or null if it names no disk (an image file, say).</summary>
    public static string? DevicePath(string arg)
    {
        if (arg.StartsWith(@"\\.\", StringComparison.Ordinal) || arg.StartsWith("//./", StringComparison.Ordinal))
            return @"\\.\" + arg[4..];
        if (arg.StartsWith("/dev/", StringComparison.Ordinal)) return arg;
        if (arg.Length > 4 && arg.StartsWith("disk", StringComparison.OrdinalIgnoreCase)
            && int.TryParse(arg.AsSpan(4), out int n) && n >= 0)
            return @"\\.\PhysicalDrive" + n;
        return null;
    }

    /// <summary>
    /// A read-only, seekable stream over the whole disk, whose Length is the
    /// disk's size. Throws <see cref="UnauthorizedAccessException"/> when the
    /// caller may not read raw disks (on Windows: not elevated).
    /// </summary>
    public static Stream OpenRead(string device)
    {
        // bufferSize 0: FileStream's own buffer would issue reads the device refuses.
        var fs = new FileStream(device, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, bufferSize: 0);
        try
        {
            long length = OperatingSystem.IsWindows() ? WindowsLength(fs.SafeFileHandle) : fs.Seek(0, SeekOrigin.End);
            fs.Seek(0, SeekOrigin.Begin);
            return new SectorAlignedStream(fs, length);
        }
        catch { fs.Dispose(); throw; }
    }

    /// <summary>
    /// The disks this machine has. Windows only, and needs no elevation: each
    /// disk is opened with no access rights, which is enough to ask its size
    /// and name. Elsewhere it is empty; use lsblk.
    /// </summary>
    public static IReadOnlyList<PhysicalDiskInfo> List()
    {
        var found = new List<PhysicalDiskInfo>();
        if (!OperatingSystem.IsWindows()) return found;
        int missing = 0;
        for (int n = 0; n < 64 && missing < 16; n++)
        {
            string path = @"\\.\PhysicalDrive" + n;
            using var h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
            if (h.IsInvalid) { missing++; continue; }
            missing = 0;
            long bytes = 0;
            try { bytes = WindowsLength(h); } catch (IOException) { }
            var (bus, removable, model) = Describe(h);
            found.Add(new PhysicalDiskInfo(n, path, bytes, bus, removable, model));
        }
        return found;
    }

    // ---- Windows ------------------------------------------------------------

    private const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
    private const uint IOCTL_DISK_GET_DRIVE_GEOMETRY_EX = 0x000700A0;   // FILE_ANY_ACCESS
    private const uint IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400;       // FILE_ANY_ACCESS

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security,
                                                     uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[]? inBuf, int inLen,
                                               byte[] outBuf, int outLen, out int returned, IntPtr overlapped);

    /// <summary>
    /// The disk's size from its geometry. A device's FileStream.Length is not
    /// its size on Windows, and the geometry IOCTL needs no read access, so
    /// the same call serves the listing and the reader.
    /// </summary>
    private static long WindowsLength(SafeFileHandle h)
    {
        // DISK_GEOMETRY_EX: a 24-byte DISK_GEOMETRY, then LARGE_INTEGER DiskSize.
        var o = new byte[256];
        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, null, 0, o, o.Length, out int got, IntPtr.Zero) || got < 32)
            throw new IOException($"the disk did not report its size (error {Marshal.GetLastWin32Error()})");
        return BitConverter.ToInt64(o, 24);
    }

    private static readonly string[] BusNames =
        ["?", "SCSI", "ATAPI", "ATA", "1394", "SSA", "Fibre", "USB", "RAID", "iSCSI", "SAS", "SATA", "SD", "MMC",
         "Virtual", "FileBacked", "Spaces", "NVMe", "SCM", "UFS"];

    /// <summary>Bus, removability and vendor + product from STORAGE_DEVICE_DESCRIPTOR.</summary>
    private static (string Bus, bool Removable, string Model) Describe(SafeFileHandle h)
    {
        var q = new byte[12];   // STORAGE_PROPERTY_QUERY: StorageDeviceProperty (0), PropertyStandardQuery (0)
        var o = new byte[1024];
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, q, q.Length, o, o.Length, out int got, IntPtr.Zero) || got < 36)
            return ("?", false, "");
        bool removable = o[10] != 0;
        uint busType = BitConverter.ToUInt32(o, 28);
        string Str(int at)
        {
            int off = BitConverter.ToInt32(o, at);
            if (off <= 0 || off >= got) return "";
            int end = Array.IndexOf(o, (byte)0, off);
            return Encoding.ASCII.GetString(o, off, (end < 0 ? got : end) - off).Trim();
        }
        string model = string.Join(' ', new[] { Str(12), Str(16) }.Where(s => s.Length > 0));
        return (busType < BusNames.Length ? BusNames[busType] : $"bus {busType}", removable, model);
    }
}

/// <summary>
/// Reads at any offset and length from a device that only accepts whole,
/// aligned sectors (a raw disk on Windows), by reading the sectors that cover
/// the request and copying out the part asked for. Read-only.
/// </summary>
public sealed class SectorAlignedStream : Stream
{
    private const int Sector = RawDisk.Sector;
    private const int Chunk = 1 << 20;
    private readonly Stream _inner;
    private readonly long _length;
    private long _pos;

    public SectorAlignedStream(Stream inner, long length)
    {
        if (length % Sector != 0) throw new ArgumentException("a disk's length is a whole number of sectors", nameof(length));
        _inner = inner;
        _length = length;
    }

    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => _length;
    public override long Position { get => _pos; set => Seek(value, SeekOrigin.Begin); }

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> dest)
    {
        if (_pos >= _length || dest.Length == 0) return 0;
        long want = Math.Min(dest.Length, Math.Min(_length - _pos, Chunk - Sector));
        long start = _pos & ~(long)(Sector - 1);
        long end = (_pos + want + Sector - 1) & ~(long)(Sector - 1);
        var tmp = new byte[end - start];

        _inner.Seek(start, SeekOrigin.Begin);
        int have = 0;
        while (have < tmp.Length)
        {
            int r = _inner.Read(tmp, have, tmp.Length - have);
            if (r <= 0) break;
            have += r;
        }
        int skip = (int)(_pos - start);
        int n = (int)Math.Clamp(have - skip, 0, want);
        tmp.AsSpan(skip, n).CopyTo(dest);
        _pos += n;
        return n;
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        long p = origin switch { SeekOrigin.Begin => offset, SeekOrigin.Current => _pos + offset, _ => _length + offset };
        if (p < 0) throw new IOException("seek before the start of the disk");
        return _pos = p;
    }

    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException("read-only");
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException("read-only");

    protected override void Dispose(bool disposing)
    {
        if (disposing) _inner.Dispose();
        base.Dispose(disposing);
    }
}
