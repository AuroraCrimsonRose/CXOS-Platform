// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;
using System.Security.Cryptography;
using CXEX.FileType.Types;

namespace CXEX.Crypto.Signing;

public static class CXVerifier
{
    /* The one permitted profile, matching SIG_ALGO_RSA2048_SHA256 and
       HASH_ALGO_SHA256 in kernel/lib/format/cxex_verify.c. Declared here
       rather than shared, so CXK does not depend on the DevKit to decide what
       CXK can verify; SignedRangeTests compares the two. */
    public const ushort SIG_ALGO_RSA2048_SHA256 = 1;
    public const ushort HASH_ALGO_SHA256 = 1;

    public enum VerifyResult
    {
        Ok = 0,
        BadSignature = -8 // Matches CXEX_VERIFY_BAD_SIGNATURE from cxex_verify.h
    }

    public static VerifyResult VerifyIntegrity(CXEXExecutable exe, byte[] rawImageBytes, XKPKFile trustedKey)
    {
        // The kernel's verify_self makes four checks before it hashes anything,
        // and this made none of them (2026-10-09 review §4). CXK is the
        // authority at the execution boundary, so none of these was a hole -
        // but "the DevKit and CXK validate independently" (§11) is the
        // invariant, and a host verifier that accepts what the kernel refuses
        // tells a developer their artifact is fine when it will not boot.

        // The signed range must lie inside the bytes handed over. `exe` and
        // `rawImageBytes` arrive as separate arguments, so a caller can pair a
        // header with the wrong buffer; the slice below would then throw
        // ArgumentOutOfRange rather than report a verdict.
        if (exe.Header is null || exe.Signature is null)
            return VerifyResult.BadSignature;
        long sigOffset = exe.Header.SignatureOffset;
        if (sigOffset <= 0 || sigOffset > rawImageBytes.Length)
            return VerifyResult.BadSignature;

        // One algorithm, as the kernel insists: sig_algo and hash_algo must
        // both be 1. This accepted any identifier and verified RSA/SHA-256
        // regardless, which is algorithm agility by omission - the thing
        // security §6 says not to have.
        if (exe.Signature.SigAlgo != SIG_ALGO_RSA2048_SHA256 ||
            exe.Signature.HashAlgo != HASH_ALGO_SHA256)
            return VerifyResult.BadSignature;

        // The signature must be exactly as long as the modulus. The kernel
        // refuses anything else (`sig_len != key.modulus_len`), and this is
        // the check that would have caught the CXSG layout disagreement Phase
        // 1 found by reading a signed image back.
        if (exe.Signature.Signature.Length != trustedKey.Modulus.Length)
            return VerifyResult.BadSignature;

        // The fingerprint must describe the key actually carried. Nothing is
        // decided by it, but a lying one puts a trusted publisher's
        // fingerprint in a log line beside an image that key never signed, so
        // the block has to be self-consistent before it names anybody.
        if (exe.Signature.PubKey.Length > 0)
        {
            byte[] fp = SHA256.HashData(exe.Signature.PubKey);
            if (!CryptographicOperations.FixedTimeEquals(fp, exe.Signature.Fingerprint))
                return VerifyResult.BadSignature;
        }

        // Hash exactly the bytes that were signed: [0, signature_offset).
        // The same range the kernel uses (`sha256(file, h->signature_offset)`),
        // and SignedRangeTests pins the two together.
        byte[] signedRegion = rawImageBytes[..(int)sigOffset];
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