using System;
using System.IO;
using System.Security.Cryptography;
using CXEX.FileType.Structures;
using CXEX.FileType.Parsers;
using CXEX.Core.Utilities;

namespace CXEX.Crypto.Signing;

public static class CXSigner
{
    private const ushort SIG_ALGO_RSA2048_SHA256 = 1;
    private const ushort HASH_ALGO_SHA256 = 1;
    private const uint FLAG_SIGNED = 1 << 2;

    public static void SignArtifact(string targetPath, string pemPrivateKeyPath, string xkpkPublicKeyPath)
    {
        byte[] binary = File.ReadAllBytes(targetPath);

        // 1. Calculate the Fingerprint of the public key
        byte[] pkBytes = File.ReadAllBytes(xkpkPublicKeyPath);
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
        }

        // 5. Append the CXSG Block
        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms);

        // Write the existing patched file
        bw.Write(binary);

        // Write the CXSG block
        bw.Write(0x47535843u); // "CXSG" Little-Endian
        bw.Write(SIG_ALGO_RSA2048_SHA256);
        bw.Write(HASH_ALGO_SHA256);
        bw.Write(fingerprint); // 32 bytes
        bw.Write((ushort)signatureBytes.Length);
        bw.Write(signatureBytes);

        File.WriteAllBytes(targetPath, ms.ToArray());
    }
}