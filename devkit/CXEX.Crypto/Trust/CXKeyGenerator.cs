using System;
using System.IO;
using System.Security.Cryptography;
using CXEX.Core.Constants;
using CXEX.Core.Utilities;

namespace CXEX.Crypto.Trust;

/// <summary>
/// RSA keypair generation for the CX trust chain. Replaces makekeys.py.
///
/// Emits two files:
///   &lt;name&gt;.xksk - PEM private signing key. NEVER ship or commit this.
///   &lt;name&gt;.xkpk - CXPK public key in CXOS's raw format, so the kernel can
///                  parse it with no ASN.1/DER parser (see rsa_parse_xkpk).
///
/// CXPK layout (16-byte header + modulus), little-endian, MUST stay exact:
///    0  4  magic "CXPK"
///    4  2  version
///    6  2  key_bits
///    8  4  exponent
///   12  2  modulus_len
///   14  2  reserved (zero)
///   16  N  modulus, big-endian (raw RSA n)
///
/// The 2 reserved bytes are NOT optional: rsa_parse_xkpk reads the modulus from
/// offset 16. A 14-byte header shifts it by two and yields a garbage key -- which
/// surfaces as CXEX_VERIFY_BAD_SIGNATURE, sending you into bignum.c for no reason.
///
/// Note also that a key's IDENTITY is sha256(these file bytes), not the key
/// material (see CXSigner / cxex_verify.c). Any change to this encoding -- even
/// the reserved field -- changes every fingerprint and invalidates every existing
/// signature. Treat the layout as frozen.
/// </summary>
public static class CXKeyGenerator
{
    public const ushort XKPK_VERSION = 1;
    private const int XKPK_HEADER_SIZE = 16;

    /// <summary>Generate a keypair and write both files. Returns the .xkpk bytes.</summary>
    public static byte[] Generate(string privateKeyPath, string publicKeyPath, int keyBits = 2048)
    {
        if (keyBits < 1024 || keyBits % 8 != 0)
            throw new ArgumentException("keyBits must be a multiple of 8 and at least 1024.", nameof(keyBits));

        using var rsa = RSA.Create(keyBits);
        RSAParameters p = rsa.ExportParameters(includePrivateParameters: false);

        byte[] modulus = p.Modulus ?? throw new CryptographicException("RSA modulus was not exported.");
        uint exponent = BigEndianToU32(p.Exponent ?? throw new CryptographicException("RSA exponent was not exported."));

        byte[] xkpk = BuildXkpk(modulus, exponent, XKPK_VERSION);

        EnsureDirectory(privateKeyPath);
        EnsureDirectory(publicKeyPath);

        // Private key as PEM. CXSigner uses RSA.ImportFromPem, which accepts both
        // PKCS#8 ("BEGIN PRIVATE KEY") and PKCS#1 ("BEGIN RSA PRIVATE KEY").
        // PKCS#8 matches what modern `openssl genrsa` emits.
        File.WriteAllText(privateKeyPath, rsa.ExportPkcs8PrivateKeyPem());
        File.WriteAllBytes(publicKeyPath, xkpk);

        return xkpk;
    }

    /// <summary>Serialize a CXPK public key. Exposed for round-trip testing.</summary>
    public static byte[] BuildXkpk(byte[] modulus, uint exponent, ushort version = XKPK_VERSION)
    {
        if (modulus.Length == 0 || modulus.Length > ushort.MaxValue)
            throw new ArgumentException("Invalid modulus length.", nameof(modulus));

        byte[] file = new byte[XKPK_HEADER_SIZE + modulus.Length];
        Span<byte> span = file;

        MemoryPrimitives.WriteU32(span, 0, CXMagic.CXPK);
        MemoryPrimitives.WriteU16(span, 4, version);
        MemoryPrimitives.WriteU16(span, 6, (ushort)(modulus.Length * 8));   // key_bits
        MemoryPrimitives.WriteU32(span, 8, exponent);
        MemoryPrimitives.WriteU16(span, 12, (ushort)modulus.Length);
        MemoryPrimitives.WriteU16(span, 14, 0);                             // reserved -- keep it

        // RSAParameters.Modulus is already big-endian, which is what the format
        // and rsa.c's bn_from_bytes both expect. Copy verbatim.
        modulus.CopyTo(span.Slice(XKPK_HEADER_SIZE));

        return file;
    }

    /// <summary>The kernel's notion of key identity: sha256 over the .xkpk file bytes.</summary>
    public static byte[] Fingerprint(byte[] xkpkFileBytes) => SHA256.HashData(xkpkFileBytes);

    private static uint BigEndianToU32(byte[] be)
    {
        if (be.Length == 0 || be.Length > 4)
            throw new CryptographicException($"Public exponent must fit in 32 bits (got {be.Length} bytes).");

        uint v = 0;
        foreach (byte b in be) v = (v << 8) | b;
        return v;
    }

    private static void EnsureDirectory(string path)
    {
        string? dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
    }
}
