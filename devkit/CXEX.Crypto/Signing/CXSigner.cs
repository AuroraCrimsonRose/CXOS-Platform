using System;
using System.IO;
using System.Security.Cryptography;
using CXEX.FileType.Structures;
using CXEX.FileType.Parsers;
using CXEX.FileType.Types;
using CXEX.Core.Utilities;

namespace CXEX.Crypto.Signing;

public static class CXSigner
{
    private const ushort SIG_ALGO_RSA2048_SHA256 = 1;
    private const ushort HASH_ALGO_SHA256 = 1;
    private const uint FLAG_SIGNED = 1 << 2;

    /// <summary>
    /// Signs a CXEX image in place.
    ///
    /// <para>The pipeline is parse, validate, then sign (security review §10). It
    /// used to be just "sign": whatever bytes were at <paramref name="targetPath"/>
    /// were hashed and a CXSG block appended. Signing is the one operation where
    /// that is least acceptable, because the output is an artifact carrying a
    /// trusted signature - a malformed image signed by the platform key is strictly
    /// worse than a malformed image.</para>
    /// </summary>
    public static void SignArtifact(string targetPath, string pemPrivateKeyPath, string xkpkPublicKeyPath)
    {
        byte[] binary = File.ReadAllBytes(targetPath);

        // 0. Validate before signing anything. CXEXExecutable.Load is the validator:
        // it refuses an image whose sections run outside the file, overlap, or fall
        // beyond the signed range.
        var image = new CXEXExecutable();
        try
        {
            image.Load(binary);
        }
        catch (InvalidDataException ex)
        {
            throw new InvalidDataException(
                $"refusing to sign {Path.GetFileName(targetPath)}: it is not a valid CXEX image. {ex.Message}", ex);
        }

        // Signing twice would hash the first signature as if it were payload and
        // leave two CXSG blocks in the file, only one of which any verifier reads.
        if (image.Header.SignatureOffset != 0)
            throw new InvalidDataException(
                $"refusing to sign {Path.GetFileName(targetPath)}: it already carries a signature at offset " +
                $"{image.Header.SignatureOffset}. Re-package it before signing again.");

        // 1. The public key, validated too - the fingerprint is taken over these
        // exact bytes, so a file that is not a CXPK yields a confident-looking
        // fingerprint for a key nobody can verify against.
        byte[] pkBytes = File.ReadAllBytes(xkpkPublicKeyPath);
        var publicKey = new XKPKFile();
        try
        {
            publicKey.Load(pkBytes);
        }
        catch (InvalidDataException ex)
        {
            throw new InvalidDataException(
                $"refusing to sign with {Path.GetFileName(xkpkPublicKeyPath)}: it is not a valid CXPK public key. {ex.Message}", ex);
        }

        if (pkBytes.Length > ushort.MaxValue)
            throw new InvalidDataException(
                $"public key is {pkBytes.Length} bytes, beyond the 16-bit pubkey_len field.");

        byte[] fingerprint = SHA256.HashData(pkBytes);

        // 2. Patch the header BEFORE hashing (just like signcxex.py)
        uint sigOffset = (uint)binary.Length;

        // Offset 12: Flags
        uint flags = MemoryPrimitives.ReadU32(binary, 12);
        flags |= FLAG_SIGNED;
        MemoryPrimitives.WriteU32(binary, 12, flags);

        // Offset 40: Signature Offset
        MemoryPrimitives.WriteU32(binary, 40, sigOffset);

        // 3. Hash the patched image
        byte[] digest = SHA256.HashData(binary);

        // 4. Native RSA Signature (No OpenSSL required)
        string pem = File.ReadAllText(pemPrivateKeyPath);
        byte[] signatureBytes;

        using (var rsa = RSA.Create())
        {
            rsa.ImportFromPem(pem);
            // PKCS1 padding matches your OpenSSL parameters
            signatureBytes = rsa.SignHash(digest, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            // The private key must be the other half of the public key travelling
            // with the image. Signing with one and shipping the other produces an
            // artifact that fails verification everywhere, with nothing in the
            // output to say why - and the kernel reports it as BAD_SIGNATURE, which
            // reads as tampering rather than as the wrong key file on a command
            // line. Caught here instead, while both halves are in hand.
            RSAParameters pub = rsa.ExportParameters(includePrivateParameters: false);
            if (!pub.Modulus.AsSpan().SequenceEqual(publicKey.Modulus))
                throw new InvalidDataException(
                    $"{Path.GetFileName(pemPrivateKeyPath)} and {Path.GetFileName(xkpkPublicKeyPath)} are not a pair: " +
                    "the private key's modulus differs from the public key's.");
        }

        // The kernel requires sig_len == modulus_len (cxex_verify.c), so an image
        // that does not satisfy it is one it will refuse to load.
        if (signatureBytes.Length != publicKey.Modulus.Length)
            throw new InvalidDataException(
                $"signature is {signatureBytes.Length} bytes but the key's modulus is {publicKey.Modulus.Length}; " +
                "the kernel requires them equal.");

        // 5. Append the CXSG Block
        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms);

        // Write the existing patched file
        bw.Write(binary);

        // Write the CXSG block.
        //
        // The signer's PUBLIC KEY travels with the image - the .xkpk file
        // verbatim, which is exactly the bytes the fingerprint above is taken
        // over. Without it a verifier that does not already hold the key can
        // check nothing at all, not even that the image is intact, so
        // "signed by someone we do not know" and "tampered with" would look
        // identical. With it, integrity is always checkable and the only open
        // question is whose key it is.
        //
        // Layout (little-endian), header 44 bytes then the two variable parts:
        //   0  "CXSG"        4
        //   4  sig_algo      2
        //   6  hash_algo     2
        //   8  fingerprint  32   sha256 of the .xkpk bytes below
        //  40  pubkey_len    2
        //  42  sig_len       2
        //  44  pubkey     pubkey_len   the .xkpk file, verbatim
        //      signature  sig_len
        bw.Write(0x47535843u); // "CXSG" Little-Endian
        bw.Write(SIG_ALGO_RSA2048_SHA256);
        bw.Write(HASH_ALGO_SHA256);
        bw.Write(fingerprint); // 32 bytes
        bw.Write((ushort)pkBytes.Length);
        bw.Write((ushort)signatureBytes.Length);
        bw.Write(pkBytes);
        bw.Write(signatureBytes);

        // Atomic, for the same reason CXEXWriter is (security §8): an interrupted
        // signing would otherwise leave the patched header - FLAG_SIGNED set and a
        // signature_offset pointing at a block that was never written.
        byte[] signed = ms.ToArray();
        string temp = targetPath + ".tmp" + Environment.ProcessId;
        try
        {
            File.WriteAllBytes(temp, signed);
            File.Move(temp, targetPath, overwrite: true);
        }
        catch
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch { /* best effort */ }
            throw;
        }
    }
}