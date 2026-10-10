// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
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
    /// <summary>
    /// The CXPK version this writes. A <c>const</c> rather than a lookup into
    /// <see cref="CXEX.Core.Constants.FormatPolicy"/> only because it is a
    /// default parameter value below, which C# requires to be a compile-time
    /// constant. The policy is still the authority: a test asserts the two are
    /// equal, so this cannot quietly disagree with it (engineering §13).
    /// </summary>
    public const ushort XKPK_VERSION = 1;
    private const int XKPK_HEADER_SIZE = 16;

    /// <summary>
    /// The one permitted cryptographic profile, declared here rather than
    /// shared with the kernel so the DevKit does not depend on CXK to decide
    /// what CXK can verify (security §12.5). These must equal
    /// <c>RSA_PROFILE_*</c> in <c>kernel/lib/crypto/rsa.h</c>; the pair is
    /// held together by the signed round-trip tests, which sign with these
    /// and are verified by the kernel's own bignum at boot.
    /// </summary>
    public const int PROFILE_KEY_BITS = 2048;
    public const int PROFILE_MODULUS_LEN = PROFILE_KEY_BITS / 8;
    public const uint PROFILE_EXPONENT = 65537;   // F4

    /// <summary>Generate a keypair and write both files. Returns the .xkpk bytes.</summary>
    public static byte[] Generate(string privateKeyPath, string publicKeyPath, int keyBits = PROFILE_KEY_BITS)
    {
        // The policy is enforced HERE, in the crypto layer, not only in the
        // CLI (security §6: "reject algorithms and parameters outside the
        // project's defined security policy rather than accepting whatever
        // the underlying framework can technically parse"). KeygenCommand
        // checked --bits and this did not, so Studio, a test or any future
        // command calling the library directly could still produce a key CXK
        // cannot verify - a key that signs perfectly and whose every artifact
        // is refused at boot as BAD_SIGNATURE, which reads as tampering
        // rather than as the wrong key size. It is also the layering rule
        // (engineering §8): the library is the authority, the front-end is a
        // front-end.
        if (keyBits != PROFILE_KEY_BITS)
            throw new CryptographicException(
                $"CXK implements RSA-{PROFILE_KEY_BITS} only; {keyBits} bits would produce a key it cannot verify.");

        using var rsa = RSA.Create(keyBits);
        RSAParameters p = rsa.ExportParameters(includePrivateParameters: false);

        byte[] modulus = p.Modulus ?? throw new CryptographicException("RSA modulus was not exported.");
        uint exponent = BigEndianToU32(p.Exponent ?? throw new CryptographicException("RSA exponent was not exported."));

        // .NET picks the exponent, and every current version picks F4 - but
        // the kernel pins it (RSA_PROFILE_EXPONENT in rsa.h) and will refuse
        // anything else, so this is checked rather than assumed. A degenerate
        // exponent is the one crypto parameter that makes verification
        // meaningless rather than merely incompatible: e = 1 makes RSA the
        // identity function and every signature forgeable.
        if (exponent != PROFILE_EXPONENT)
            throw new CryptographicException(
                $"CXK pins the public exponent to {PROFILE_EXPONENT}; this key uses {exponent}.");

        // Deliberately redundant with the keyBits check above: that one is a
        // request filter, this one is a fact about what was produced. They
        // overlap for every size, and the sabotage run showed it - restoring
        // the old permissive `keyBits >= 1024` check alone changed nothing,
        // because a 1024-bit key still failed here on its 128-byte modulus.
        // Both are kept: the first refuses before spending the key
        // generation and names the real reason, the second is the one that
        // would still catch a framework that honoured the request loosely.
        if (modulus.Length != PROFILE_MODULUS_LEN)
            throw new CryptographicException(
                $"expected a {PROFILE_MODULUS_LEN}-byte modulus for RSA-{PROFILE_KEY_BITS}, got {modulus.Length}.");

        byte[] xkpk = BuildXkpk(modulus, exponent, XKPK_VERSION);

        EnsureDirectory(privateKeyPath);
        EnsureDirectory(publicKeyPath);

        // The PUBLIC half first, deliberately. If the second write fails, the
        // leftover on disk is then a public key with no private half - which
        // is useless to everyone - rather than a private signing key with
        // nothing to pair it with, sitting in a directory the caller may not
        // know it needs to clean up (security §7, temporary-file leakage).
        File.WriteAllBytes(publicKeyPath, xkpk);

        // Private key as PEM. CXSigner uses RSA.ImportFromPem, which accepts both
        // PKCS#8 ("BEGIN PRIVATE KEY") and PKCS#1 ("BEGIN RSA PRIVATE KEY").
        // PKCS#8 matches what modern `openssl genrsa` emits.
        //
        // Created OWNER-ONLY, and at creation rather than afterwards
        // (security §7, "overly broad filesystem permissions"). File.Write*
        // creates with the default mode, which umask usually leaves
        // world-readable, so a private signing key was readable by every user
        // on the machine for the whole life of the file. Setting the mode
        // after the write would leave a window in which it was not; passing
        // it in UnixCreateMode means the file never exists with any other
        // mode.
        //
        // Set only off Windows, and that is not defensive tidiness: the
        // UnixCreateMode *setter* throws PlatformNotSupportedException on
        // Windows rather than ignoring the value, so assigning it
        // unconditionally breaks `cxk keygen` on the platform this is
        // developed on. On Windows the inherited directory ACL governs
        // instead, which is why the permission test says so rather than
        // passing quietly there.
        var opts = new FileStreamOptions
        {
            Mode = FileMode.Create,
            Access = FileAccess.Write,
        };
        if (!OperatingSystem.IsWindows())
            opts.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;

        using (var writer = new StreamWriter(new FileStream(privateKeyPath, opts)))
            writer.Write(rsa.ExportPkcs8PrivateKeyPem());

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
