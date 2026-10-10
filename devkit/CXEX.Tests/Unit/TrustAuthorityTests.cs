// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Core.Constants;
using CXEX.Crypto.Signing;
using CXEX.Crypto.Trust;
using CXEX.FileType.Types;
using CXEX.Tests.Adversarial;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The authority invariant: a trust level follows from <b>which key signed</b>,
/// and from nothing the artifact says about itself (DevKit security review §4,
/// §5).
///
/// <para>The review's concern is that "a publisher key must not accidentally
/// become capable of signing platform-authoritative artifacts merely because
/// the same cryptographic implementation can process both". It had. The old
/// <c>CXAuthChain.AuthorizeLoad</c> checked that <i>some</i> key in the store
/// had signed the image and then granted "Kernel/Executive Privilege" on
/// finding <c>FLAG_KERNEL_PRIV</c> set in the header - a bit the image
/// declares about itself. Any imported publisher key could therefore produce
/// an image it called kernel-privileged.</para>
///
/// <para>It was never reachable - nothing called it, and the kernel defines
/// <c>CXEX_FLAG_KERNEL_PRIV</c> without ever reading it - so this closes a
/// wrong model rather than a live hole. These tests are what keep it closed,
/// and they are written against the properties rather than the implementation:
/// each one would fail on the old code.</para>
/// </summary>
public class TrustAuthorityTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("trust_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
        GC.SuppressFinalize(this);
    }

    /// <summary>Signs a fresh image with a fresh key pair. Returns the image path,
    /// the raw bytes, and the public key's bytes as they travel in the image.</summary>
    private (string Path, byte[] Raw, byte[] Xkpk, string XkpkPath) Sign(uint extraFlags = 0)
    {
        string tag = Guid.NewGuid().ToString("N")[..8];
        string sk = Path.Combine(_dir, $"{tag}.xksk");
        string pk = Path.Combine(_dir, $"{tag}.xkpk");
        byte[] xkpk = CXKeyGenerator.Generate(sk, pk);

        string image = Path.Combine(_dir, $"{tag}.xuex");
        var builder = CxexBuilder.Valid();
        if (extraFlags != 0) builder.Flags |= extraFlags;
        File.WriteAllBytes(image, builder.Build());
        CXSigner.SignArtifact(image, sk, pk);

        return (image, File.ReadAllBytes(image), xkpk, pk);
    }

    private static (CXEXExecutable Exe, byte[] Raw) Load(string path)
    {
        byte[] raw = File.ReadAllBytes(path);
        var exe = new CXEXExecutable();
        exe.Load(raw);
        return (exe, raw);
    }

    // ---- the property the review asks for ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_publisher_key_cannot_produce_a_platform_tier_artifact()
    {
        // Two independent key pairs: one is the platform's, one a publisher's.
        var platform = Sign();
        var publisher = Sign();

        // The publisher's key is imported - the strongest thing a developer,
        // a build script or an installer can do - and the platform key is
        // configured. Importing makes a publisher; it must never make the
        // platform.
        var store = new CXKeyStore();
        store.ImportKey(publisher.XkpkPath);
        var chain = new CXAuthChain(store, platform.Xkpk);

        var (exe, raw) = Load(publisher.Path);
        var level = chain.Evaluate(exe, raw, out var failure);

        Assert.Equal(CXTrustLevel.Publisher, level);
        Assert.NotEqual(CXTrustLevel.Platform, level);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_platform_key_reaches_platform_so_this_is_not_passing_by_refusing_everything()
    {
        var platform = Sign();

        var store = new CXKeyStore();
        var chain = new CXAuthChain(store, platform.Xkpk);

        var (exe, raw) = Load(platform.Path);
        Assert.Equal(CXTrustLevel.Platform, chain.Evaluate(exe, raw, out _));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_self_declared_privilege_flag_does_not_raise_the_trust_level()
    {
        // The exact shape of the old defect: a publisher-signed image that
        // declares itself kernel-privileged. The flag must change nothing.
        var platform = Sign();
        var privileged = Sign(CXFlags.FLAG_KERNEL_PRIV);

        var store = new CXKeyStore();
        store.ImportKey(privileged.XkpkPath);
        var chain = new CXAuthChain(store, platform.Xkpk);

        var (exe, raw) = Load(privileged.Path);
        Assert.True((exe.Header.Flags & CXFlags.FLAG_KERNEL_PRIV) != 0,
                    "the test image does not actually carry the flag, so this proves nothing");
        Assert.Equal(CXTrustLevel.Publisher, chain.Evaluate(exe, raw, out _));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_unknown_key_is_unverified_rather_than_trusted_or_tampered()
    {
        var platform = Sign();
        var stranger = Sign();

        // Nothing imported: the signature is valid and the signer is nobody
        // this installation knows. That is a third answer, not a failure.
        var chain = new CXAuthChain(new CXKeyStore(), platform.Xkpk);

        var (exe, raw) = Load(stranger.Path);
        Assert.Equal(CXTrustLevel.Unverified, chain.Evaluate(exe, raw, out _));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void With_no_platform_key_configured_nothing_reaches_platform()
    {
        // Absence is never promoted to a pass. A null platform key used to be
        // the kind of thing that made a comparison vacuously true.
        var signed = Sign();

        var store = new CXKeyStore();
        store.ImportKey(signed.XkpkPath);
        var chain = new CXAuthChain(store, platformKeyBytes: null);

        var (exe, raw) = Load(signed.Path);
        var level = chain.Evaluate(exe, raw, out _);
        Assert.NotEqual(CXTrustLevel.Platform, level);
        Assert.Equal(CXTrustLevel.Publisher, level);
    }

    // ---- integrity comes first, whoever signed ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_tampered_image_is_refused_even_when_the_platform_key_signed_it()
    {
        var platform = Sign();

        byte[] raw = File.ReadAllBytes(platform.Path);
        var exe = new CXEXExecutable();
        exe.Load(raw);

        // Flip a byte of payload, well clear of the header and the signature.
        byte[] tampered = (byte[])raw.Clone();
        int at = (int)(exe.Header.SignatureOffset / 2);
        tampered[at] ^= 0xFF;

        var chain = new CXAuthChain(new CXKeyStore(), platform.Xkpk);
        var level = chain.Evaluate(exe, tampered, out var failure);

        Assert.Null(level);
        Assert.Equal(CXAuthFailure.BadSignature, failure);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_unsigned_image_is_refused_and_named_as_unsigned()
    {
        string image = Path.Combine(_dir, "bare.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid().Build());

        var (exe, raw) = Load(image);
        var chain = new CXAuthChain(new CXKeyStore(), new byte[] { 1, 2, 3 });

        Assert.Null(chain.Evaluate(exe, raw, out var failure));
        Assert.Equal(CXAuthFailure.Unsigned, failure);
    }

    // ---- §5: Secure Boot trust is a different domain ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Secure_boot_key_material_cannot_be_imported_as_a_cxex_trust_key()
    {
        // §5 asks that a key trusted to authenticate firmware not become a key
        // authorized to sign user executables. The separation here is
        // structural: Secure Boot uses X.509 (`cxk secureboot keygen` writes
        // .pem/.cer), CXEX uses CXPK, and the CXPK parser enforces one profile
        // - so Secure Boot material cannot even be loaded into the key store,
        // let alone trusted by it. Asserted rather than assumed, because the
        // failure mode would be silent: a store that accepted a PEM would
        // index it under a fingerprint and happily report it trusted.
        string pem = Path.Combine(_dir, "sb.pem");
        File.WriteAllText(pem,
            "-----BEGIN CERTIFICATE-----\n" +
            Convert.ToBase64String(new byte[256]) + "\n" +
            "-----END CERTIFICATE-----\n");

        var store = new CXKeyStore();
        Assert.ThrowsAny<Exception>(() => store.ImportKey(pem));
    }
}
