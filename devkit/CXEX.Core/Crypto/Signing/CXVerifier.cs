using System.Security.Cryptography;
using CXEX.Core.FileTypes;

namespace CXEX.Core.Crypto.Signing;

public static class CXVerifier
{
    /// <summary>
    /// Verifies an image against the signature block.
    /// Mirroring kernel: cxex_verify(const uint8_t *file, size_t len, ...)
    /// </summary>
    public static bool Verify(byte[] fileData, int signatureOffset, CXSignature sig, ICXPublicKey pubKey)
    {
        // 1. Integrity: Hash exactly the bytes that were signed [0, signature_offset)
        // This is exactly what cxex_verify.c does: sha256(file, h.signature_offset, digest);
        ReadOnlySpan<byte> signedRegion = fileData.AsSpan(0, signatureOffset);
        byte[] digest = SHA256.HashData(signedRegion);

        // 2. RSA Verify: Ensure signature matches the digest using the provided public key
        // This validates: (Signature)^e mod n == Hash
        return pubKey.Verify(digest, sig.SignatureBytes);
    }
}