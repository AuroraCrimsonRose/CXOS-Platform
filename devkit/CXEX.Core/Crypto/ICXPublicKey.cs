namespace CXEX.Core.Crypto;

public interface ICXPublicKey
{
    CXKeyType Algorithm { get; }
    CXKeyRole Role { get; }

    // Returns the SHA256 fingerprint for verification/trust-anchoring
    byte[] GetFingerprint();

    // Verifies data against a signature
    bool Verify(byte[] data, byte[] signature);
}