// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Security.Cryptography;
using CXEX.Crypto.Signing;
using CXEX.Crypto.Trust;
using CXEX.FileType.Types;
using CXEX.Tests;
using CXEX.Tests.Adversarial;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The host and the kernel must authenticate the same bytes under the same
/// rules (2026-10-09 review §4).
///
/// <para>Phase 1 found and fixed the one real disagreement - the CXSG block
/// layout - and <c>CxsgRoundTripTests</c> pins that. What nothing asserted was
/// the <i>signed range</i> and the <i>policy</i> around it: both sides hash
/// <c>[0, signature_offset)</c>, but by coincidence of two separately written
/// expressions rather than by anything holding them together.</para>
///
/// <para>Checking it turned up a real asymmetry rather than a disagreement
/// about the range. <c>verify_self</c> makes four checks before it hashes -
/// algorithm identifiers, signature length against the modulus, fingerprint
/// consistency, and a parseable key - and <c>CXVerifier.VerifyIntegrity</c>
/// made <b>none</b> of them. CXK is the authority so nothing was exploitable,
/// but a host verifier that accepts what the kernel refuses tells a developer
/// their artifact is fine when it will not boot, and §11 asks the two to
/// validate independently rather than for one to be a weaker copy.</para>
/// </summary>
public class SignedRangeTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("signrange_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
        GC.SuppressFinalize(this);
    }

    private (CXEXExecutable Exe, byte[] Raw, XKPKFile Key) SignFresh()
    {
        string sk = Path.Combine(_dir, "k.xksk");
        string pk = Path.Combine(_dir, "k.xkpk");
        byte[] xkpk = CXKeyGenerator.Generate(sk, pk);

        string image = Path.Combine(_dir, "a.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid().Build());
        CXSigner.SignArtifact(image, sk, pk);

        byte[] raw = File.ReadAllBytes(image);
        var exe = new CXEXExecutable();
        exe.Load(raw);

        var key = new XKPKFile();
        key.Load(xkpk);
        return (exe, raw, key);
    }

    // ---- the range itself ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Both_sides_hash_the_image_up_to_the_signature_offset()
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) { Assert.True(true, "no checkout: skipped"); return; }

        // The kernel's rule, read from its source rather than restated here.
        // If verify_self starts hashing a different range, this fails - which
        // is the only thing that makes "they agree" an assertion rather than
        // a hope.
        string verify = Path.Combine(repo, "kernel", "lib", "format", "cxex_verify.c");
        if (!File.Exists(verify)) { Assert.True(true, "source not found: skipped"); return; }
        Assert.Contains("sha256(file, h->signature_offset, digest)", File.ReadAllText(verify));

        // And the host's: the digest over [0, signature_offset) is the one the
        // signature verifies against.
        var (exe, raw, key) = SignFresh();
        byte[] expected = SHA256.HashData(raw[..(int)exe.Header.SignatureOffset]);

        Assert.Equal(CXVerifier.VerifyResult.Ok, CXVerifier.VerifyIntegrity(exe, raw, key));

        // A byte flipped INSIDE the range breaks it; one flipped AFTER it does
        // not. That is what "the signed range ends at signature_offset" means,
        // and it is checked from both directions so the range is pinned rather
        // than merely named.
        byte[] inside = (byte[])raw.Clone();
        inside[(int)exe.Header.SignatureOffset / 2] ^= 0xFF;
        Assert.NotEqual(expected, SHA256.HashData(inside[..(int)exe.Header.SignatureOffset]));
        Assert.Equal(CXVerifier.VerifyResult.BadSignature,
                     CXVerifier.VerifyIntegrity(exe, inside, key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_section_lies_inside_the_signed_range()
    {
        // The invariant Phase 1 added on both sides: a section whose file
        // bytes reach past signature_offset is data nobody signed. Asserted
        // here on a real signed image, not only on adversarial ones.
        var (exe, _, _) = SignFresh();
        foreach (var s in exe.Sections)
        {
            if (s.FileSize == 0) continue;
            Assert.True(s.FileOffset + s.FileSize <= exe.Header.SignatureOffset,
                        $"section at {s.FileOffset}+{s.FileSize} reaches past the signed range");
        }
    }

    // ---- the policy the kernel applies and the host did not ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_profile_constants_match_the_kernels()
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) { Assert.True(true, "no checkout: skipped"); return; }
        string verify = Path.Combine(repo, "kernel", "lib", "format", "cxex_verify.c");
        if (!File.Exists(verify)) { Assert.True(true, "source not found: skipped"); return; }

        string text = File.ReadAllText(verify);
        Assert.Contains($"#define SIG_ALGO_RSA2048_SHA256  {CXVerifier.SIG_ALGO_RSA2048_SHA256}", text);
        Assert.Contains($"#define HASH_ALGO_SHA256         {CXVerifier.HASH_ALGO_SHA256}", text);
    }

    [Theory]
    [InlineData((ushort)0)]
    [InlineData((ushort)2)]
    [InlineData((ushort)0xFFFF)]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_unexpected_signature_algorithm_is_refused(ushort algo)
    {
        // The kernel returns CXEX_VERIFY_BAD_ALGO for these. The host used to
        // ignore the field entirely and verify RSA/SHA-256 regardless, which
        // is algorithm agility by omission.
        var (exe, raw, key) = SignFresh();
        exe.Signature!.SigAlgo = algo;
        Assert.Equal(CXVerifier.VerifyResult.BadSignature,
                     CXVerifier.VerifyIntegrity(exe, raw, key));
    }

    [Theory]
    [InlineData((ushort)0)]
    [InlineData((ushort)2)]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_unexpected_hash_algorithm_is_refused(ushort algo)
    {
        var (exe, raw, key) = SignFresh();
        exe.Signature!.HashAlgo = algo;
        Assert.Equal(CXVerifier.VerifyResult.BadSignature,
                     CXVerifier.VerifyIntegrity(exe, raw, key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_signature_length_that_is_not_the_modulus_length_is_refused()
    {
        // `sig_len != key.modulus_len` in the kernel. This is the check that
        // would have caught the CXSG layout disagreement Phase 1 found by
        // reading a signed image back - it reported a 272-byte signature where
        // RSA-2048 produces 256.
        var (exe, raw, key) = SignFresh();
        Assert.Equal(256, exe.Signature!.Signature.Length);

        exe.Signature.Signature = exe.Signature.Signature[..255];
        Assert.Equal(CXVerifier.VerifyResult.BadSignature,
                     CXVerifier.VerifyIntegrity(exe, raw, key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_fingerprint_that_does_not_describe_the_carried_key_is_refused()
    {
        var (exe, raw, key) = SignFresh();
        exe.Signature!.Fingerprint[0] ^= 0xFF;
        Assert.Equal(CXVerifier.VerifyResult.BadSignature,
                     CXVerifier.VerifyIntegrity(exe, raw, key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_signature_offset_outside_the_buffer_is_a_verdict_not_a_crash()
    {
        // exe and rawImageBytes arrive as separate arguments, so a caller can
        // pair a header with the wrong buffer. The slice used to throw
        // ArgumentOutOfRange instead of returning a result.
        var (exe, raw, key) = SignFresh();
        byte[] truncated = raw[..(int)(exe.Header.SignatureOffset / 2)];

        Assert.Equal(CXVerifier.VerifyResult.BadSignature,
                     CXVerifier.VerifyIntegrity(exe, truncated, key));
    }
}
