using System;
using System.Security.Cryptography;
using CXEX.FileType.Types;

namespace CXEX.Crypto.Signing;

public static class CXVerifier
{
    public enum VerifyResult
    {
        Ok = 0,
        BadSignature = -8 // Matches CXEX_VERIFY_BAD_SIGNATURE from cxex_verify.h
    }

    public static VerifyResult VerifyIntegrity(CXEXExecutable exe, byte[] rawImageBytes, XKPKFile trustedKey)
    {
        // Hash exactly the bytes that were signed: [0, signature_offset)
        byte[] signedRegion = rawImageBytes[..(int)exe.Header.SignatureOffset];
        byte[] digest = SHA256.HashData(signedRegion);

        // Hydrate the C# RSA engine with your custom public key struct
        var rsaParams = new RSAParameters
        {
            Modulus = trustedKey.Modulus,
            Exponent = BitConverter.GetBytes(trustedKey.Header.Exponent)
        };

        // .NET RSA expects Big-Endian parameter arrays; CXEX stores Little-Endian
        Array.Reverse(rsaParams.Exponent);
        if (rsaParams.Exponent[0] == 0)
            rsaParams.Exponent = rsaParams.Exponent[1..]; // Trim leading zeroes for .NET strictness

        using var rsa = RSA.Create();
        rsa.ImportParameters(rsaParams);

        // Verify the SHA-256 digest using PKCS1 padding (matching your OpenSSL command)
        bool isValid = rsa.VerifyHash(digest, exe.Signature!.Signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        return isValid ? VerifyResult.Ok : VerifyResult.BadSignature;
    }
}