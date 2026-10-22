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

    public static bool Unchanged(string path, byte[] hash)
    {
        using var f = File.OpenRead(path);
        return f.Length == SizeBytes && SHA256.HashData(f).AsSpan().SequenceEqual(hash);
    }

    /// <summary>How many disks the kernel said it found, from its boot log,
    /// or -1. Under two, the foreign disk was never seen and its being
    /// unchanged proves nothing.</summary>
    public static int DisksSeen(string log)
    {
        Match m = DisksLine().Match(log);
        return m.Success ? int.Parse(m.Groups[1].Value) : -1;
    }

    [GeneratedRegex(@"online, disks: (\d+)")]
    private static partial Regex DisksLine();
}
