// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace CXEX.Tools;

/// <summary>
/// The disk `cxk verify-boot --foreign-disk` puts in front of CXOS: a stand-in
/// for a drive in a machine CXOS is booted on, which must come out of the boot
/// exactly as it went in (CXK_AUTO_INSTALL, kernel/config.h).
///
/// <para>The hardest such drive: one with CXOS on it already. It is the image
/// being booted, copied, with a different XBPT disk_id - so it has a BOOT, a
/// SYSTEM waiting for its first-boot install, and a CRASH area, and it comes
/// first. A kernel that took the first CXOS disk for its own would install
/// into it; one that looked for a crash area on any disk would use its
/// one.</para>
///
/// <para>After the copy, <see cref="PadBytes"/> of pattern, not zeros: a
/// write of zeros to a zeroed disk would change nothing and pass. The padding
/// also gives the disk a size the image does not have, by which the kernel's
/// log names it.</para>
/// </summary>
public static class ForeignDisk
{
    public const int PadBytes = 8 * 1024 * 1024;

    // The XBPT header's disk_id (kernel/drivers/storage/partition/partition.h):
    // LBA 1, byte 20.
    public const int DiskIdAt = 512 + 20;

    /// <summary>Writes the disk, a copy of <paramref name="cxosImage"/> under
    /// another disk_id, and returns its SHA-256.</summary>
    public static byte[] Create(string path, ReadOnlySpan<byte> cxosImage)
    {
        var bytes = new byte[cxosImage.Length + PadBytes];
        cxosImage.CopyTo(bytes);
        Span<byte> id = bytes.AsSpan(DiskIdAt, 8);
        ulong other = ~BitConverter.ToUInt64(id);
        BitConverter.TryWriteBytes(id, other == 0 ? 1UL : other);
        for (int i = cxosImage.Length; i < bytes.Length; i++) bytes[i] = (byte)(i * 7 + i / 512);
        File.WriteAllBytes(path, bytes);
        return SHA256.HashData(bytes);
    }

    /// <summary>True if the disk is as Create left it, false if it changed,
    /// null if it could not be read back (still held open, say): not a
    /// verdict either way, and the caller must not call it a pass.</summary>
    public static bool? Unchanged(string path, byte[] hash)
    {
        try
        {
            // ReadWrite: QEMU may not have let go of the file yet, and
            // reading must not throw for that.
            using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return SHA256.HashData(f).AsSpan().SequenceEqual(hash);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    /// <summary>Whether the kernel's boot log names this disk among those it
    /// left read-only, by its size, which no other disk in the run shares.
    /// Without that line the disk was never seen, and its being unchanged
    /// proves nothing. A count of disks is not enough: it cannot tell this
    /// disk from some other one.</summary>
    public static bool Seen(string log, long sizeBytes) =>
        Regex.IsMatch(log, $@"left read-only, sectors: {sizeBytes / 512}\b");
}
