// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.FileType.Types;
using CXEX.Crypto.Signing;

namespace CXEX.Crypto.Trust;

/// <summary>
/// How far an image's signer is trusted, once integrity has been established.
/// Mirrors the kernel's <c>enum cx_trust</c> (kernel/cpu/keyvault.h) value for
/// value, and is ordered so that higher is more trusted and a policy reads as
/// a minimum.
/// </summary>
public enum CXTrustLevel
{
    /// <summary>Valid signature by a key this installation has not been told to believe.</summary>
    Unverified = 0,
    /// <summary>The signing key is in the key store - someone decided to believe this publisher.</summary>
    Publisher = 1,
    /// <summary>The signing key is the platform key itself.</summary>
    Platform = 2,
}

/// <summary>
/// Why an image could not be trusted at all. Negative, like the kernel's
/// <c>cxex_verify_result</c>, because "did not verify" is a different kind of
/// answer from "verified, and here is who signed it".
/// </summary>
public enum CXAuthFailure
{
    Unsigned = -1,
    BadSignature = -2,
    BadKey = -3,
    Malformed = -4,
}

/// <summary>
/// The DevKit's view of the trust decision CXK makes at the execution
/// boundary. Advisory only: CXK is the authority (DevKit security review
/// §12.5), and this exists so tooling can tell a developer what the kernel
/// WILL decide, without ever becoming the thing that decides.
///
/// AUTHORITY INVARIANT (DevKit security review §4, §5)
///
/// A trust level is <b>derived from which key signed</b>, and from nothing
/// else. It is never read out of the artifact. There is deliberately no
/// authority, tier or privilege field in CXEX for a signer to fill in, so
/// there is nothing for a publisher to claim and nothing for a verifier to
/// have to disbelieve. The review asks for authority transitions to be
/// "explicit rather than inferred from a key's mere presence"; the stronger
/// property available here is that the transition cannot be expressed in the
/// artifact in the first place.
///
/// This class previously did the opposite, and it is worth recording what was
/// wrong with it. It granted "Kernel/Executive Privilege" on finding
/// FLAG_KERNEL_PRIV set in the image header - a bit the image declares about
/// itself - after checking only that *some* key in the store had signed it.
/// Any publisher key a developer had imported could therefore produce an
/// image this class called kernel-privileged, which is precisely the
/// confusion §4 names. It was never reachable: nothing called it, and the
/// kernel defines CXEX_FLAG_KERNEL_PRIV without ever reading it, so no
/// decision anywhere rested on the flag. It is replaced rather than deleted
/// because the question it was trying to answer is a real one.
///
/// FLAG_KERNEL_PRIV is still not consulted below, and that is the point: what
/// an image may do follows from who signed it (kernel/cpu/exec.c decides that
/// from the trust level), not from what the image says about itself.
/// </summary>
public class CXAuthChain
{
    private readonly CXKeyStore _keyStore;
    private readonly byte[]? _platformKey;

    /// <param name="keyStore">Publisher keys: the DevKit's stand-in for /System/KeyVault.</param>
    /// <param name="platformKeyBytes">
    /// The raw bytes of the platform public key - the exact contents of the
    /// .xkpk the kernel was built with (tools/kernel.xkpk by default). Null
    /// means this installation has no platform key configured, in which case
    /// no image can reach Platform: absence is never promoted to a pass.
    /// </param>
    public CXAuthChain(CXKeyStore keyStore, byte[]? platformKeyBytes = null)
    {
        _keyStore = keyStore;
        _platformKey = platformKeyBytes;
    }

    /// <summary>
    /// Establishes integrity against the key the image itself carries, then
    /// says how far that key is trusted. Returns a <see cref="CXTrustLevel"/>,
    /// or null with <paramref name="failure"/> set if the image does not
    /// verify at all.
    /// </summary>
    public CXTrustLevel? Evaluate(CXEXExecutable exe, byte[] rawExecutableBytes,
                                  out CXAuthFailure failure)
    {
        failure = CXAuthFailure.Malformed;

        // INTEGRITY first, always, and against the key the image carries.
        // Nothing below is worth asking until the bytes are known to be what
        // the signer signed - the same ordering keyvault_trust_of uses.
        if (exe.Header is null || !exe.Header.IsSigned || exe.Signature is null)
        {
            failure = CXAuthFailure.Unsigned;
            return null;
        }

        byte[] carriedKey = exe.Signature.PubKey;
        if (carriedKey is null || carriedKey.Length == 0)
        {
            failure = CXAuthFailure.BadKey;
            return null;
        }

        XKPKFile parsedKey;
        try
        {
            parsedKey = new XKPKFile();
            parsedKey.Load(carriedKey);
        }
        catch (Exception)
        {
            // A key that will not parse under the one permitted profile is
            // not a key. Reported as such rather than thrown, so a caller
            // sees a verdict instead of a stack trace.
            failure = CXAuthFailure.BadKey;
            return null;
        }

        if (CXVerifier.VerifyIntegrity(exe, rawExecutableBytes, parsedKey)
            != CXVerifier.VerifyResult.Ok)
        {
            failure = CXAuthFailure.BadSignature;
            return null;
        }

        // IDENTITY, most trusted first.
        //
        // The platform key is compared BYTE FOR BYTE and not looked up in the
        // store, for the same reason the kernel compiles it in rather than
        // reading it from /System/KeyVault: a key store is something a
        // developer, a build script or an installer can add to, and the
        // platform authority must not be reachable that way. Importing a key
        // can make a publisher; it can never make the platform.
        if (_platformKey is not null && _platformKey.Length > 0 &&
            CryptographicEquals(carriedKey, _platformKey))
            return CXTrustLevel.Platform;

        if (_keyStore.IsTrusted(exe.Signature.Fingerprint))
            return CXTrustLevel.Publisher;

        // Signed, intact, and by someone this installation has never been
        // told to believe. A real answer, and not the same as tampered.
        return CXTrustLevel.Unverified;
    }

    /// <summary>
    /// Human-readable form of <see cref="Evaluate"/>, for tooling output.
    /// </summary>
    public string Describe(CXEXExecutable exe, byte[] rawExecutableBytes)
    {
        var level = Evaluate(exe, rawExecutableBytes, out var failure);
        if (level is null)
        {
            return failure switch
            {
                CXAuthFailure.Unsigned => "REFUSED: unsigned, or no CXSG block.",
                CXAuthFailure.BadSignature => "REFUSED: signature does not verify over these bytes.",
                CXAuthFailure.BadKey => "REFUSED: the carried key is malformed or outside the permitted profile.",
                _ => "REFUSED: malformed image.",
            };
        }

        return level switch
        {
            CXTrustLevel.Platform => "PLATFORM: signed by the platform key.",
            CXTrustLevel.Publisher => "PUBLISHER: signed by a key in the key store.",
            _ => "UNVERIFIED: valid signature, by a key this installation does not know.",
        };
    }

    /// <summary>
    /// Length-independent, constant-time-per-byte comparison. Trust is decided
    /// by exact key bytes (security §12.4), so this is a trust-root comparison
    /// and does not get to short-circuit on the first difference.
    /// </summary>
    private static bool CryptographicEquals(byte[] a, byte[] b)
    {
        if (a.Length != b.Length) return false;
        int diff = 0;
        for (int i = 0; i < a.Length; i++) diff |= a[i] ^ b[i];
        return diff == 0;
    }
}
