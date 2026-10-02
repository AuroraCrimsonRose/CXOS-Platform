using System.Security.Cryptography;
using CXEX.Core.Utilities;
using CXEX.Crypto.Signing;
using CXEX.Crypto.Trust;
using CXEX.FileType.Parsers;
using CXEX.FileType.Types;
using Xunit;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// The crypto cases of DevKit security review §14: malformed and undersized
/// keys, malformed signatures, wrong algorithm identifiers, corrupted key
/// identifiers, and signatures over modified metadata or payload.
///
/// <para>Everything here runs in-process - RSA key generation included - so it
/// needs no toolchain and no built OS.</para>
/// </summary>
public class CryptoTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cxcrypto_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
        GC.SuppressFinalize(this);
    }

    private string P(string name) => Path.Combine(_dir, name);

    /// <summary>A real keypair and a freshly signed, valid image.</summary>
    private (string Image, string Sk, string Pk, byte[] Xkpk) Signed()
    {
        string sk = P("k.xksk"), pk = P("k.xkpk");
        byte[] xkpk = CXKeyGenerator.Generate(sk, pk);

        string image = P("a.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid().Build());
        CXSigner.SignArtifact(image, sk, pk);
        return (image, sk, pk, xkpk);
    }

    // ---- public keys ----

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_public_key_without_the_cxpk_magic_is_rejected()
    {
        byte[] key = CXKeyGenerator.BuildXkpk(new byte[256], 65537);
        key[0] = (byte)'Z';

        var f = new XKPKFile();
        Assert.Throws<InvalidDataException>(() => f.Load(key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_truncated_public_key_is_rejected()
    {
        byte[] key = CXKeyGenerator.BuildXkpk(new byte[256], 65537);

        var f = new XKPKFile();
        Assert.Throws<InvalidDataException>(() => f.Load(key.AsSpan(0, 8).ToArray()));    // short header
        Assert.Throws<InvalidDataException>(() => f.Load(key.AsSpan(0, 100).ToArray()));  // short modulus
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void An_oversized_modulus_is_rejected()
    {
        // Beyond what the kernel's rsa_verify_sha256 will accept, so a key the
        // DevKit took would produce images the kernel refuses.
        byte[] key = CXKeyGenerator.BuildXkpk(new byte[256], 65537);
        MemoryPrimitives.WriteU16(key.AsSpan(), 12, 512);   // modulus_len
        MemoryPrimitives.WriteU16(key.AsSpan(), 6, 512 * 8);

        var f = new XKPKFile();
        Assert.Throws<InvalidDataException>(() => f.Load(key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_zero_length_modulus_is_rejected()
    {
        byte[] key = CXKeyGenerator.BuildXkpk(new byte[256], 65537);
        MemoryPrimitives.WriteU16(key.AsSpan(), 12, 0);
        MemoryPrimitives.WriteU16(key.AsSpan(), 6, 0);

        var f = new XKPKFile();
        Assert.Throws<InvalidDataException>(() => f.Load(key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Key_bits_disagreeing_with_modulus_len_is_rejected()
    {
        byte[] key = CXKeyGenerator.BuildXkpk(new byte[256], 65537);
        MemoryPrimitives.WriteU16(key.AsSpan(), 6, 1024);   // says 1024-bit, carries 2048

        var f = new XKPKFile();
        Assert.Throws<InvalidDataException>(() => f.Load(key));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_degenerate_public_exponent_is_rejected()
    {
        foreach (uint bad in new uint[] { 0, 1, 4 })
        {
            byte[] key = CXKeyGenerator.BuildXkpk(new byte[256], 65537);
            MemoryPrimitives.WriteU32(key.AsSpan(), 8, bad);

            var f = new XKPKFile();
            Assert.Throws<InvalidDataException>(() => f.Load(key));
        }
    }

    // ---- signing refuses bad input ----

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Signing_a_malformed_image_is_refused()
    {
        string sk = P("k.xksk"), pk = P("k.xkpk");
        CXKeyGenerator.Generate(sk, pk);

        // A section claiming more bytes than the file holds.
        string image = P("bad.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid()
            .WithSection(0, s => { s.FileSize = 0x10_0000; s.MemSize = 0x10_0000; s.Payload = Array.Empty<byte>(); })
            .Build());

        var ex = Assert.Throws<InvalidDataException>(() => CXSigner.SignArtifact(image, sk, pk));
        Assert.Contains("refusing to sign", ex.Message);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Signing_with_a_malformed_public_key_is_refused()
    {
        string sk = P("k.xksk"), pk = P("k.xkpk");
        CXKeyGenerator.Generate(sk, pk);
        File.WriteAllBytes(pk, "not a key"u8.ToArray());

        string image = P("a.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid().Build());

        Assert.Throws<InvalidDataException>(() => CXSigner.SignArtifact(image, sk, pk));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Signing_with_a_mismatched_key_pair_is_refused()
    {
        string skA = P("a.xksk"), pkA = P("a.xkpk");
        string skB = P("b.xksk"), pkB = P("b.xkpk");
        CXKeyGenerator.Generate(skA, pkA);
        CXKeyGenerator.Generate(skB, pkB);

        string image = P("a.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid().Build());

        // Private half of one pair, public half of the other. Without this check
        // it signs happily and the artifact fails verification everywhere, which
        // the kernel reports as BAD_SIGNATURE - indistinguishable from tampering.
        var ex = Assert.Throws<InvalidDataException>(() => CXSigner.SignArtifact(image, skA, pkB));
        Assert.Contains("not a pair", ex.Message);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void Signing_an_already_signed_image_is_refused()
    {
        (string image, string sk, string pk, _) = Signed();

        var ex = Assert.Throws<InvalidDataException>(() => CXSigner.SignArtifact(image, sk, pk));
        Assert.Contains("already carries a signature", ex.Message);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_refused_signing_leaves_the_image_untouched()
    {
        string skA = P("a.xksk"), pkA = P("a.xkpk"), pkB = P("b.xkpk");
        CXKeyGenerator.Generate(skA, pkA);
        CXKeyGenerator.Generate(P("b.xksk"), pkB);

        string image = P("a.xuex");
        byte[] before = CxexBuilder.Valid().Build();
        File.WriteAllBytes(image, before);

        Assert.Throws<InvalidDataException>(() => CXSigner.SignArtifact(image, skA, pkB));

        Assert.Equal(before, File.ReadAllBytes(image));
        Assert.Empty(Directory.GetFiles(_dir, "*.tmp*"));
    }

    // ---- tampering with a signed image ----

    /// <summary>
    /// The signature covers [0, signature_offset), so a changed payload byte must
    /// break it. Without this the suite proves only that signing produces bytes,
    /// not that those bytes mean anything.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_signature_does_not_verify_over_a_modified_payload()
    {
        (string image, _, _, _) = Signed();
        byte[] signed = File.ReadAllBytes(image);

        Assert.True(VerifiesAgainstCarriedKey(signed), "the freshly signed image did not verify");

        // Flip a byte of section data, inside the signed range.
        var exe = new CXEXExecutable();
        exe.Load(signed);
        signed[exe.Sections[0].FileOffset] ^= 0xFF;

        Assert.False(VerifiesAgainstCarriedKey(signed), "a modified payload still verified");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_signature_does_not_verify_over_modified_metadata()
    {
        (string image, _, _, _) = Signed();
        byte[] signed = File.ReadAllBytes(image);

        // entry_point, at offset 16 - metadata, not payload, and inside the
        // signed range. An image whose entry point can be moved after signing is
        // an image whose signature means nothing.
        MemoryPrimitives.WriteU32(signed.AsSpan(), 16, 0xDEAD_B000);

        Assert.False(VerifiesAgainstCarriedKey(signed), "modified metadata still verified");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_corrupted_fingerprint_no_longer_identifies_the_carried_key()
    {
        (string image, _, _, byte[] xkpk) = Signed();
        byte[] signed = File.ReadAllBytes(image);

        var header = CXEXParser.ParseHeader(signed);
        var sig = CXEXParser.ParseSignature(signed, header.SignatureOffset);
        Assert.Equal(SHA256.HashData(xkpk), sig.Fingerprint);

        // The kernel checks sha256(carried key) == fingerprint precisely so a
        // trusted publisher's fingerprint cannot be pinned to someone else's key.
        signed[header.SignatureOffset + 8] ^= 0xFF;

        var tampered = CXEXParser.ParseSignature(signed, header.SignatureOffset);
        Assert.NotEqual(SHA256.HashData(tampered.PubKey), tampered.Fingerprint);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_malformed_signature_length_is_rejected()
    {
        (string image, _, _, _) = Signed();
        byte[] signed = File.ReadAllBytes(image);
        var header = CXEXParser.ParseHeader(signed);

        // sig_len at +42 of the block, claiming far more than the file holds.
        MemoryPrimitives.WriteU16(signed.AsSpan(), (int)header.SignatureOffset + 42, 0xFFFF);

        Assert.Throws<InvalidDataException>(() => CXEXParser.ParseSignature(signed, header.SignatureOffset));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_wrong_algorithm_identifier_is_visible_to_the_reader()
    {
        (string image, _, _, _) = Signed();
        byte[] signed = File.ReadAllBytes(image);
        var header = CXEXParser.ParseHeader(signed);

        // The kernel fixes RSA-2048/SHA-256 and refuses anything else
        // (cxex_verify.c). The reader must at least report what it was told, so a
        // caller can refuse it rather than assume the algorithm it expected.
        MemoryPrimitives.WriteU16(signed.AsSpan(), (int)header.SignatureOffset + 4, 7);
        MemoryPrimitives.WriteU16(signed.AsSpan(), (int)header.SignatureOffset + 6, 9);

        var sig = CXEXParser.ParseSignature(signed, header.SignatureOffset);
        Assert.Equal(7, sig.SigAlgo);
        Assert.Equal(9, sig.HashAlgo);
        Assert.NotEqual(1, sig.SigAlgo);
    }

    /// <summary>
    /// Verifies the image with the public key it carries, the way a verifier that
    /// does not already hold the key must.
    /// </summary>
    private static bool VerifiesAgainstCarriedKey(byte[] signed)
    {
        var header = CXEXParser.ParseHeader(signed);
        var sig = CXEXParser.ParseSignature(signed, header.SignatureOffset);

        byte[] digest = SHA256.HashData(signed.AsSpan(0, (int)header.SignatureOffset).ToArray());

        var key = new XKPKFile();
        key.Load(sig.PubKey);

        byte[] exponent = BitConverter.GetBytes(key.Header.Exponent);
        Array.Reverse(exponent);
        int lead = 0;
        while (lead < exponent.Length - 1 && exponent[lead] == 0) lead++;

        using var rsa = RSA.Create();
        rsa.ImportParameters(new RSAParameters { Modulus = key.Modulus, Exponent = exponent[lead..] });
        return rsa.VerifyHash(digest, sig.Signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    }
}
