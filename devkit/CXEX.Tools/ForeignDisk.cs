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
/// <para>Filled with a pattern, not zeros: a write of zeros to a zeroed disk
/// would change nothing and pass. It holds no CXFS and no XBPT table, so to a
/// kernel claiming disks it looks like a blank drive to format.</para>
/// </summary>
public static partial class ForeignDisk
{
    // Bigger than anything the kernel formats (a 16 MB "Data" volume), so a
    // format is not stopped by the disk being too small to hold it.
    public const int SizeBytes = 32 * 1024 * 1024;

    /// <summary>Writes the disk and returns its SHA-256.</summary>
    public static byte[] Create(string path)
    {
        var bytes = new byte[SizeBytes];
        for (int i = 0; i < bytes.Length; i++) bytes[i] = (byte)(i * 7 + i / 512);
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
            return f.Length == SizeBytes && SHA256.HashData(f).AsSpan().SequenceEqual(hash);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    /// <summary>Whether the kernel's boot log names this disk among those it
    /// left read-only, by its size, which no other disk in the run shares.
    /// Without that line the disk was never seen, and its being unchanged
    /// proves nothing. A count of disks is not enough: it cannot tell this
    /// disk from some other one.</summary>
    public static bool Seen(string log) => RefusedLine().IsMatch(log);

    [GeneratedRegex(@"left read-only, sectors: 65536\b")]
    private static partial Regex RefusedLine();
}
