// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Buffers.Binary;
using System.Security.Cryptography;

namespace CXEX.Uefi.SecureBoot;

/// <summary>
/// Builds EFI_SIGNATURE_LIST blobs, the payload format of PK, KEK, db and dbx.
/// </summary>
/// <remarks>
/// <para>
/// EFI_SIGNATURE_LIST {
///   EFI_GUID SignatureType;      // what the entries are: a cert, or a hash
///   UINT32   SignatureListSize;  // the WHOLE list including this header
///   UINT32   SignatureHeaderSize;// 0 for both types we emit
///   UINT32   SignatureSize;      // ONE entry: 16-byte owner GUID + payload
///   EFI_SIGNATURE_DATA Signatures[];
/// }
/// </para>
/// <para>
/// SignatureSize is per-entry, not total, which means every entry in one list
/// must be the same length. Certificates are not, so each certificate gets its
/// own list and the lists are concatenated. That is legal and is what real
/// stores look like; trying to pack differently-sized certs into one list
/// produces a blob firmware silently mis-parses.
/// </para>
/// </remarks>
public static class EfiSignatureList
{
    private const int ListHeaderSize = 16 + 4 + 4 + 4;   // 28
    private const int OwnerSize = 16;

    /// <summary>Wraps one DER certificate as a single-entry EFI_CERT_X509 list.</summary>
    public static byte[] FromCertificate(byte[] der, Guid owner)
        => Build(EfiGuids.CertX509, owner, der);

    /// <summary>Wraps one SHA-256 digest as a single-entry EFI_CERT_SHA256 list.</summary>
    public static byte[] FromSha256(byte[] digest, Guid owner)
    {
        if (digest.Length != 32)
            throw new ArgumentException($"a SHA-256 entry must be 32 bytes, got {digest.Length}", nameof(digest));
        return Build(EfiGuids.CertSha256, owner, digest);
    }

    /// <summary>
    /// The placeholder dbx every real store carries: the SHA-256 of the empty
    /// string. dbx must exist for firmware to consider the platform configured,
    /// but it must not actually revoke anything - and no image can ever hash to
    /// the digest of no input, so this entry is unmatched by construction.
    /// </summary>
    public static byte[] EmptyDbx()
        => FromSha256(SHA256.HashData([]), EfiGuids.DummyOwner);

    /// <summary>Concatenates several lists into one variable payload.</summary>
    public static byte[] Concat(IEnumerable<byte[]> lists)
    {
        var all = lists.ToArray();
        var outBuf = new byte[all.Sum(l => l.Length)];
        int at = 0;
        foreach (var l in all) { l.CopyTo(outBuf, at); at += l.Length; }
        return outBuf;
    }

    private static byte[] Build(Guid type, Guid owner, byte[] payload)
    {
        int sigSize = OwnerSize + payload.Length;
        var buf = new byte[ListHeaderSize + sigSize];
        var s = buf.AsSpan();

        type.ToByteArray().CopyTo(s);
        BinaryPrimitives.WriteUInt32LittleEndian(s[16..], (uint)buf.Length);   // SignatureListSize
        BinaryPrimitives.WriteUInt32LittleEndian(s[20..], 0);                 // SignatureHeaderSize
        BinaryPrimitives.WriteUInt32LittleEndian(s[24..], (uint)sigSize);     // SignatureSize
        owner.ToByteArray().CopyTo(s[ListHeaderSize..]);
        payload.CopyTo(s[(ListHeaderSize + OwnerSize)..]);
        return buf;
    }
}
