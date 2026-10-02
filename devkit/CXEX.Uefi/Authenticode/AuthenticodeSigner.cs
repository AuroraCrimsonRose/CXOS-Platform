// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Buffers.Binary;
using System.Formats.Asn1;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace CXEX.Uefi.Authenticode;

/// <summary>
/// Signs and verifies PE images with Authenticode - the signature format UEFI
/// Secure Boot checks a bootloader against.
/// </summary>
/// <remarks>
/// <para>
/// This is written directly against System.Formats.Asn1 rather than SignedCms,
/// for two reasons. SignedCms lives in a NuGet package, and more importantly it
/// encodes eContent as an OCTET STRING as RFC 5652 requires - while Authenticode
/// puts the SpcIndirectDataContent SEQUENCE in raw. A SignedCms signature is
/// structurally valid CMS that every Secure Boot verifier rejects.
/// </para>
/// <para>
/// Two details here are not derivable from the published grammar and were
/// established by comparing against signatures real tools produce:
/// </para>
/// <list type="number">
/// <item>
/// The <c>file</c> field of SpcPeImageData is <c>[0] EXPLICIT</c>. The published
/// SpcPeImageData shows it untagged; Microsoft's own template tags it, and
/// parsers fail with a wrong-tag error without it.
/// </item>
/// <item>
/// The messageDigest attribute is computed over the SpcIndirectDataContent's
/// VALUE OCTETS - its outer SEQUENCE tag and length excluded. Hashing the full
/// DER yields a signature that is mathematically correct and rejected everywhere
/// with "digest failure".
/// </item>
/// </list>
/// </remarks>
public static class AuthenticodeSigner
{
    private const string OidSignedData   = "1.2.840.113549.1.7.2";
    private const string OidRsaEncryption = "1.2.840.113549.1.1.1";
    private const string OidContentType  = "1.2.840.113549.1.9.3";
    private const string OidMessageDigest = "1.2.840.113549.1.9.4";
    private const string OidSpcIndirectData = "1.3.6.1.4.1.311.2.1.4";
    private const string OidSpcPeImageData  = "1.3.6.1.4.1.311.2.1.15";

    private const ushort WinCertRevision2 = 0x0200;
    private const ushort WinCertTypePkcsSignedData = 0x0002;

    /// <summary>Signs <paramref name="pe"/> and returns the signed file.</summary>
    public static byte[] Sign(PeImage pe, X509Certificate2 cert)
    {
        using var rsa = cert.GetRSAPrivateKey()
            ?? throw new CryptographicException(
                "the signing certificate has no usable RSA private key - a .cer or .pem holds only " +
                "the public half; sign with the .pfx.");

        byte[] imageHash = pe.ComputeHash(HashAlgorithmName.SHA256);
        byte[] spc = BuildSpcIndirectDataContent(imageHash);

        // Encoded ONCE as a universal SET OF. The signature covers these exact
        // bytes; SignerInfo carries the same bytes re-tagged [0] IMPLICIT. Encoding
        // twice would risk the two differing, since DER SET OF ordering would have
        // to come out identical both times.
        byte[] attributes = BuildAuthenticatedAttributes(spc);
        byte[] signature = rsa.SignData(attributes, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        byte[] signedData = BuildSignedData(spc, attributes, signature, cert);
        return pe.WithCertificateTable(WrapWinCertificate(signedData));
    }

    /// <summary>
    /// Checks a signed PE: that the embedded digest matches the image, and that the
    /// signature verifies under <paramref name="trusted"/>.
    /// </summary>
    /// <remarks>
    /// Deliberately not a full Authenticode implementation - it does not walk a
    /// chain or check revocation. It answers the question that matters before
    /// booting: would firmware holding this certificate in db accept this file.
    /// </remarks>
    public static bool Verify(PeImage pe, X509Certificate2 trusted, out string reason)
    {
        reason = "";
        byte[]? p7 = pe.ExtractSignature();
        if (p7 is null) { reason = "the file has no certificate table (it is unsigned)"; return false; }

        byte[] spc, attributes, signature;
        try
        {
            (spc, attributes, signature) = ParseSignedData(p7);
        }
        catch (Exception ex)
        {
            reason = $"the signature could not be parsed: {ex.Message}";
            return false;
        }

        byte[] embedded;
        try
        {
            embedded = ExtractImageHash(spc);
        }
        catch (Exception ex)
        {
            reason = $"the SpcIndirectDataContent could not be read: {ex.Message}";
            return false;
        }

        byte[] actual = pe.ComputeHash(HashAlgorithmName.SHA256);
        if (!CryptographicOperations.FixedTimeEquals(embedded, actual))
        {
            reason = $"image hash mismatch - the file was modified after signing " +
                     $"(signed {Convert.ToHexString(embedded)}, actual {Convert.ToHexString(actual)})";
            return false;
        }

        byte[] attrDigest = SHA256.HashData(SpcValueOctets(spc));
        if (!CryptographicOperations.FixedTimeEquals(ExtractMessageDigest(attributes), attrDigest))
        {
            reason = "the messageDigest attribute does not match the signed content";
            return false;
        }

        using var pub = trusted.GetRSAPublicKey();
        if (pub is null) { reason = "the trusted certificate has no RSA public key"; return false; }
        if (!pub.VerifyData(attributes, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1))
        {
            reason = "the signature was not made by the given certificate";
            return false;
        }
        return true;
    }

    // ---- encoding ---------------------------------------------------------

    private static void WriteAlgorithmId(AsnWriter w, string oid)
    {
        using (w.PushSequence()) { w.WriteObjectIdentifier(oid); w.WriteNull(); }
    }

    private static byte[] BuildSpcIndirectDataContent(byte[] imageHash)
    {
        var w = new AsnWriter(AsnEncodingRules.DER);
        using (w.PushSequence())
        {
            using (w.PushSequence())
            {
                w.WriteObjectIdentifier(OidSpcPeImageData);
                using (w.PushSequence())
                {
                    w.WriteBitString(new byte[] { 0x00 }, 0);        // flags, unused in practice
                    // file [0] EXPLICIT SpcLink -> file [2] EXPLICIT SpcString
                    //   -> unicode [0] IMPLICIT BMPString
                    using (w.PushSequence(new Asn1Tag(TagClass.ContextSpecific, 0, true)))
                    using (w.PushSequence(new Asn1Tag(TagClass.ContextSpecific, 2, true)))
                        w.WriteCharacterString(UniversalTagNumber.BMPString, "<<<Obsolete>>>",
                            new Asn1Tag(TagClass.ContextSpecific, 0));
                }
            }
            using (w.PushSequence())                                // DigestInfo
            {
                WriteAlgorithmId(w, Oids.Sha256);
                w.WriteOctetString(imageHash);
            }
        }
        return w.Encode();
    }

    private static byte[] BuildAuthenticatedAttributes(byte[] spc)
    {
        var w = new AsnWriter(AsnEncodingRules.DER);
        using (w.PushSetOf())
        {
            using (w.PushSequence())
            {
                w.WriteObjectIdentifier(OidContentType);
                using (w.PushSetOf()) w.WriteObjectIdentifier(OidSpcIndirectData);
            }
            using (w.PushSequence())
            {
                w.WriteObjectIdentifier(OidMessageDigest);
                using (w.PushSetOf()) w.WriteOctetString(SHA256.HashData(SpcValueOctets(spc)));
            }
        }
        return w.Encode();
    }

    private static byte[] BuildSignedData(byte[] spc, byte[] attributes, byte[] signature,
                                          X509Certificate2 cert)
    {
        // Same bytes as were signed, re-tagged from SET (0x31) to [0] IMPLICIT
        // (0xA0). Only the tag byte changes; the length encoding is untouched.
        byte[] attrsImplicit = (byte[])attributes.Clone();
        attrsImplicit[0] = 0xA0;

        var w = new AsnWriter(AsnEncodingRules.DER);
        using (w.PushSequence())                                    // ContentInfo
        {
            w.WriteObjectIdentifier(OidSignedData);
            using (w.PushSequence(new Asn1Tag(TagClass.ContextSpecific, 0, true)))
            using (w.PushSequence())                                // SignedData
            {
                w.WriteInteger(1);
                using (w.PushSetOf()) WriteAlgorithmId(w, Oids.Sha256);
                using (w.PushSequence())                            // EncapsulatedContentInfo
                {
                    w.WriteObjectIdentifier(OidSpcIndirectData);
                    // RAW. No OCTET STRING wrapper - see the class remarks.
                    using (w.PushSequence(new Asn1Tag(TagClass.ContextSpecific, 0, true)))
                        w.WriteEncodedValue(spc);
                }
                using (w.PushSetOf(new Asn1Tag(TagClass.ContextSpecific, 0, true)))
                    w.WriteEncodedValue(cert.RawData);
                using (w.PushSetOf())                               // SignerInfos
                using (w.PushSequence())
                {
                    w.WriteInteger(1);
                    using (w.PushSequence())                        // IssuerAndSerialNumber
                    {
                        w.WriteEncodedValue(cert.IssuerName.RawData);
                        w.WriteIntegerUnsigned(cert.SerialNumberBytes.Span);
                    }
                    WriteAlgorithmId(w, Oids.Sha256);
                    w.WriteEncodedValue(attrsImplicit);
                    WriteAlgorithmId(w, OidRsaEncryption);
                    w.WriteOctetString(signature);
                }
            }
        }
        return w.Encode();
    }

    private static byte[] WrapWinCertificate(byte[] signedData)
    {
        // dwLength covers the header plus the blob but NOT the padding that aligns
        // the next table; the table as a whole is padded to 8 bytes.
        int pad = (8 - (signedData.Length % 8)) % 8;
        var buf = new byte[8 + signedData.Length + pad];
        BinaryPrimitives.WriteInt32LittleEndian(buf, 8 + signedData.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(4), WinCertRevision2);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(6), WinCertTypePkcsSignedData);
        signedData.CopyTo(buf, 8);
        return buf;
    }

    // ---- decoding ---------------------------------------------------------

    /// <summary>Strips a DER value's tag and length, leaving the value octets.</summary>
    /// <remarks>
    /// The length header is one byte below 128 and 1 + n in the long form, so the
    /// total header is not reliably two bytes.
    /// </remarks>
    internal static ReadOnlySpan<byte> SpcValueOctets(byte[] der)
    {
        int i = 1;                                  // SEQUENCE has a single-byte tag
        int len = der[i++];
        if ((len & 0x80) != 0) i += len & 0x7F;
        return der.AsSpan(i);
    }

    private static (byte[] Spc, byte[] Attributes, byte[] Signature) ParseSignedData(byte[] p7)
    {
        var outer = new AsnReader(p7, AsnEncodingRules.BER).ReadSequence();
        if (outer.ReadObjectIdentifier() != OidSignedData)
            throw new InvalidDataException("not a PKCS#7 signedData");

        var signedData = outer.ReadSequence(new Asn1Tag(TagClass.ContextSpecific, 0, true)).ReadSequence();
        signedData.ReadInteger();                                    // version
        signedData.ReadSetOf();                                     // digestAlgorithms

        var encap = signedData.ReadSequence();
        if (encap.ReadObjectIdentifier() != OidSpcIndirectData)
            throw new InvalidDataException("content type is not SPC_INDIRECT_DATA");
        var eContent = encap.ReadSequence(new Asn1Tag(TagClass.ContextSpecific, 0, true));
        byte[] spc = eContent.ReadEncodedValue().ToArray();

        // certificates [0] IMPLICIT and crls [1] IMPLICIT are both optional
        if (signedData.PeekTag() == new Asn1Tag(TagClass.ContextSpecific, 0, true))
            signedData.ReadEncodedValue();
        if (signedData.PeekTag() == new Asn1Tag(TagClass.ContextSpecific, 1, true))
            signedData.ReadEncodedValue();

        var signerInfo = signedData.ReadSetOf().ReadSequence();
        signerInfo.ReadInteger();                                   // version
        signerInfo.ReadEncodedValue();                              // IssuerAndSerialNumber
        signerInfo.ReadSequence();                                  // digestAlgorithm

        byte[] attributes = signerInfo.ReadEncodedValue().ToArray();
        if (attributes.Length == 0 || attributes[0] != 0xA0)
            throw new InvalidDataException("signerInfo has no authenticatedAttributes");
        attributes[0] = 0x31;                                       // back to SET, as signed

        signerInfo.ReadSequence();                                  // digestEncryptionAlgorithm
        byte[] signature = signerInfo.ReadOctetString();
        return (spc, attributes, signature);
    }

    private static byte[] ExtractImageHash(byte[] spc)
    {
        var seq = new AsnReader(spc, AsnEncodingRules.BER).ReadSequence();
        seq.ReadEncodedValue();                                     // SpcAttributeTypeAndOptionalValue
        var digestInfo = seq.ReadSequence();
        digestInfo.ReadSequence();                                  // AlgorithmIdentifier
        return digestInfo.ReadOctetString();
    }

    private static byte[] ExtractMessageDigest(byte[] attributes)
    {
        var set = new AsnReader(attributes, AsnEncodingRules.BER).ReadSetOf();
        while (set.HasData)
        {
            var attr = set.ReadSequence();
            string oid = attr.ReadObjectIdentifier();
            var values = attr.ReadSetOf();
            if (oid == OidMessageDigest) return values.ReadOctetString();
        }
        throw new InvalidDataException("no messageDigest attribute");
    }

    private static class Oids { internal const string Sha256 = "2.16.840.1.101.3.4.2.1"; }
}
