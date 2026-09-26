using System.Buffers.Binary;
using System.Security.Cryptography;

namespace CXEX.Uefi.Authenticode;

/// <summary>
/// The parts of a PE32/PE32+ file Authenticode cares about: where the certificate
/// table lives, and the image hash.
/// </summary>
public sealed class PeImage
{
    public byte[] Bytes { get; }
    public bool IsPe32Plus { get; }
    public int ChecksumOffset { get; }
    public int SecurityDirOffset { get; }
    public int SizeOfHeaders { get; }
    public int CertTableOffset { get; }
    public int CertTableSize { get; }
    public IReadOnlyList<(int Pointer, int Size)> Sections { get; }

    private PeImage(byte[] bytes, bool pe32Plus, int checksumOffset, int securityDirOffset,
                    int sizeOfHeaders, int certOffset, int certSize,
                    List<(int, int)> sections)
    {
        Bytes = bytes; IsPe32Plus = pe32Plus; ChecksumOffset = checksumOffset;
        SecurityDirOffset = securityDirOffset; SizeOfHeaders = sizeOfHeaders;
        CertTableOffset = certOffset; CertTableSize = certSize; Sections = sections;
    }

    public static PeImage Load(string path) => Parse(File.ReadAllBytes(path));

    public static PeImage Parse(byte[] pe)
    {
        if (pe.Length < 0x40 || pe[0] != (byte)'M' || pe[1] != (byte)'Z')
            throw new InvalidDataException("not a PE file (no MZ signature)");

        int lfanew = BinaryPrimitives.ReadInt32LittleEndian(pe.AsSpan(0x3C));
        if (lfanew <= 0 || lfanew + 24 > pe.Length)
            throw new InvalidDataException($"implausible e_lfanew 0x{lfanew:X}");
        if (BinaryPrimitives.ReadUInt32LittleEndian(pe.AsSpan(lfanew)) != 0x00004550)   // "PE\0\0"
            throw new InvalidDataException("missing PE signature");

        int coff = lfanew + 4;
        int numSections = BinaryPrimitives.ReadUInt16LittleEndian(pe.AsSpan(coff + 2));
        int sizeOfOptHdr = BinaryPrimitives.ReadUInt16LittleEndian(pe.AsSpan(coff + 16));
        int opt = coff + 20;
        ushort magic = BinaryPrimitives.ReadUInt16LittleEndian(pe.AsSpan(opt));
        bool pe32Plus = magic switch
        {
            0x20B => true,
            0x10B => false,
            _ => throw new InvalidDataException($"unknown optional header magic 0x{magic:X4}"),
        };

        // CheckSum sits at a fixed offset in both optional header shapes; the data
        // directories start after the fixed part, which is where they differ.
        int checksumOffset = opt + 64;
        int sizeOfHeaders = BinaryPrimitives.ReadInt32LittleEndian(pe.AsSpan(opt + 60));
        int dataDirs = opt + (pe32Plus ? 112 : 96);
        int securityDir = dataDirs + 4 * 8;            // IMAGE_DIRECTORY_ENTRY_SECURITY
        if (securityDir + 8 > pe.Length)
            throw new InvalidDataException("optional header is truncated before the security directory");

        int certOffset = BinaryPrimitives.ReadInt32LittleEndian(pe.AsSpan(securityDir));
        int certSize = BinaryPrimitives.ReadInt32LittleEndian(pe.AsSpan(securityDir + 4));

        int secTable = opt + sizeOfOptHdr;
        var sections = new List<(int, int)>();
        for (int i = 0; i < numSections; i++)
        {
            int s = secTable + i * 40;
            if (s + 40 > pe.Length) throw new InvalidDataException("section table is truncated");
            int rawSize = BinaryPrimitives.ReadInt32LittleEndian(pe.AsSpan(s + 16));
            int rawPtr = BinaryPrimitives.ReadInt32LittleEndian(pe.AsSpan(s + 20));
            if (rawSize > 0) sections.Add((rawPtr, rawSize));
        }
        // Authenticode hashes sections in FILE order, which is not necessarily the
        // order of the section table.
        sections.Sort((a, b) => a.Item1.CompareTo(b.Item1));

        return new PeImage(pe, pe32Plus, checksumOffset, securityDir, sizeOfHeaders,
                           certOffset, certSize, sections);
    }

    /// <summary>End of the signed content: the certificate table if present, else EOF.</summary>
    public int SignedLength => CertTableOffset != 0 && CertTableSize != 0 ? CertTableOffset : Bytes.Length;

    /// <summary>
    /// The Authenticode image hash.
    /// </summary>
    /// <remarks>
    /// Three regions are excluded, and all three matter: the CheckSum field
    /// (signing changes the file, so the checksum cannot be part of what is
    /// signed), the security data directory entry (it points at the signature
    /// being created), and the certificate table itself.
    /// </remarks>
    public byte[] ComputeHash(HashAlgorithmName algorithm)
    {
        using var hash = IncrementalHash.CreateHash(algorithm);
        void Eat(int off, int len)
        {
            if (len <= 0) return;
            if (off < 0 || off + len > Bytes.Length)
                throw new InvalidDataException($"hash region 0x{off:X}+0x{len:X} is outside the file");
            hash.AppendData(Bytes, off, len);
        }

        Eat(0, ChecksumOffset);
        Eat(ChecksumOffset + 4, SecurityDirOffset - (ChecksumOffset + 4));
        Eat(SecurityDirOffset + 8, SizeOfHeaders - (SecurityDirOffset + 8));

        int hashed = SizeOfHeaders;
        foreach (var (ptr, size) in Sections) { Eat(ptr, size); hashed += size; }

        // Anything appended past the last section is part of the image and is
        // hashed too - except the certificate table, which is where we stop.
        int end = SignedLength;
        if (end > hashed) Eat(hashed, end - hashed);

        return hash.GetHashAndReset();
    }

    /// <summary>
    /// Replaces the certificate table with <paramref name="winCert"/> and returns
    /// the new file. Any existing signature is dropped rather than appended to;
    /// firmware only ever looks at the first signature, so a second one is dead
    /// weight that also breaks the hash of the first.
    /// </summary>
    public byte[] WithCertificateTable(byte[] winCert)
    {
        int at = SignedLength;
        while (at % 8 != 0) at++;                  // the table must be 8-byte aligned
        var outBuf = new byte[at + winCert.Length];
        Bytes.AsSpan(0, Math.Min(Bytes.Length, at)).CopyTo(outBuf);
        winCert.CopyTo(outBuf, at);
        BinaryPrimitives.WriteInt32LittleEndian(outBuf.AsSpan(SecurityDirOffset), at);
        BinaryPrimitives.WriteInt32LittleEndian(outBuf.AsSpan(SecurityDirOffset + 4), winCert.Length);
        return outBuf;
    }

    /// <summary>The PKCS#7 blob inside the certificate table, or null if unsigned.</summary>
    public byte[]? ExtractSignature()
    {
        if (CertTableOffset == 0 || CertTableSize == 0) return null;
        if (CertTableOffset + 8 > Bytes.Length) throw new InvalidDataException("certificate table is out of range");
        int dwLength = BinaryPrimitives.ReadInt32LittleEndian(Bytes.AsSpan(CertTableOffset));
        if (dwLength < 8 || CertTableOffset + dwLength > Bytes.Length)
            throw new InvalidDataException($"bad WIN_CERTIFICATE length {dwLength}");
        ushort type = BinaryPrimitives.ReadUInt16LittleEndian(Bytes.AsSpan(CertTableOffset + 6));
        if (type != 0x0002)
            throw new InvalidDataException($"certificate type 0x{type:X4} is not PKCS_SIGNED_DATA");
        return Bytes.AsSpan(CertTableOffset + 8, dwLength - 8).ToArray();
    }
}
