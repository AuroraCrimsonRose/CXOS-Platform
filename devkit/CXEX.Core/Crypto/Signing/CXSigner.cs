using System.Security.Cryptography;

namespace CXEX.Core.Crypto.Signing;

public static class CXSigner
{
    public static byte[] SignData(byte[] dataToSign, ICXPrivateKey privateKey)
    {
        // 1. Compute SHA256 Hash
        byte[] hash = SHA256.HashData(dataToSign);

        // 2. Perform RSA Sign using the provided private key implementation
        return privateKey.Sign(hash);
    }
}