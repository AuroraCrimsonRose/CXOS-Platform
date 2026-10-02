using System.Security.Cryptography;
using CXEX.Crypto.Signing;
using CXEX.Crypto.Trust;
using CXEX.FileType.Parsers;
using CXEX.Tests.Adversarial;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The signer and the reader must agree about the CXSG block, byte for byte.
///
/// <para>They did not. <c>CXEXParser</c> treated the header as 42 bytes and read
/// <c>sig_len</c> from offset 40 - which is <c>pubkey_len</c> - so on a real signed
/// image it reported a 272-byte signature where RSA-2048 produces 256, and placed
/// the signature two bytes before the public key. Nothing caught it because every
/// image built in this repository is unsigned by default, so the signed path was
/// never read back.</para>
///
/// <para>These round-trip through the real <see cref="CXKeyGenerator"/> and
/// <see cref="CXSigner"/>, so they would have failed on the bug and will fail again
/// if either side of the layout moves. Everything is in-process .NET - RSA key
/// generation included - so this stays a Unit test with no toolchain requirement.</para>
/// </summary>
public class CxsgRoundTripTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cxsg_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
        GC.SuppressFinalize(this);
    }

    private (string Image, byte[] Xkpk) SignFreshImage()
    {
        string sk = Path.Combine(_dir, "k.xksk");
        string pk = Path.Combine(_dir, "k.xkpk");
        byte[] xkpk = CXKeyGenerator.Generate(sk, pk);

        string image = Path.Combine(_dir, "hi.xuex");
        File.WriteAllBytes(image, CxexBuilder.Valid().Build());
        CXSigner.SignArtifact(image, sk, pk);

        return (image, xkpk);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Signer_and_parser_agree_on_the_cxsg_layout()
    {
        (string image, byte[] xkpk) = SignFreshImage();
        byte[] signed = File.ReadAllBytes(image);

        var header = CXEXParser.ParseHeader(signed);
        Assert.True(header.SignatureOffset > 0, "signing left signature_offset at 0");

        var sig = CXEXParser.ParseSignature(signed, header.SignatureOffset);

        // The sizes are the whole bug: 272 is the .xkpk's length, 256 is an
        // RSA-2048 signature. Reading one where the other belongs is what made a
        // signed image verify over the wrong bytes.
        Assert.Equal(xkpk.Length, sig.PubKeyLen);
        Assert.Equal(256, sig.SigLen);
        Assert.NotEqual(sig.PubKeyLen, sig.SigLen);

        // The key travels with the image verbatim, and the fingerprint is taken
        // over exactly those bytes - that is the kernel's notion of key identity
        // (cxex_verify.c hashes pubkey_len bytes and compares to fingerprint).
        Assert.Equal(xkpk, sig.PubKey);
        Assert.Equal(SHA256.HashData(xkpk), sig.Fingerprint);
    }

    /// <summary>
    /// The kernel refuses the image unless <c>sig_len</c> equals the modulus length
    /// (<c>cxex_verify.c</c>: <c>if (sig->sig_len != key.modulus_len)</c>), so the
    /// signature the reader hands back must actually verify against the digest the
    /// signer took. This is the end-to-end check the sizes above only imply.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_parsed_signature_verifies_against_the_signed_region()
    {
        (string image, _) = SignFreshImage();
        byte[] signed = File.ReadAllBytes(image);

        var header = CXEXParser.ParseHeader(signed);
        var sig = CXEXParser.ParseSignature(signed, header.SignatureOffset);

        // Exactly the bytes the signer hashed: [0, signature_offset).
        byte[] digest = SHA256.HashData(signed.AsSpan(0, (int)header.SignatureOffset).ToArray());

        // Rebuild the public key from the .xkpk carried in the block: 16-byte
        // header then a big-endian modulus (CXKeyGenerator documents the layout).
        byte[] modulus = sig.PubKey.AsSpan(16).ToArray();
        uint exponent = (uint)(sig.PubKey[8] | (sig.PubKey[9] << 8) | (sig.PubKey[10] << 16) | (sig.PubKey[11] << 24));

        byte[] exponentBe = BitConverter.GetBytes(exponent);
        Array.Reverse(exponentBe);
        int lead = 0;
        while (lead < exponentBe.Length - 1 && exponentBe[lead] == 0) lead++;

        using var rsa = RSA.Create();
        rsa.ImportParameters(new RSAParameters { Modulus = modulus, Exponent = exponentBe[lead..] });

        Assert.True(
            rsa.VerifyHash(digest, sig.Signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1),
            "the signature read out of the CXSG block does not verify over [0, signature_offset) - " +
            "the reader and the signer disagree about where the signature starts or how long it is");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_signature_offset_past_the_end_is_rejected()
    {
        byte[] image = CxexBuilder.Valid().Build();

        // 0xFFFFFFFF casts to -1 as an int, and a bounds test written as
        // "length < offset + 44" then passes for any file over 43 bytes before
        // indexing at -1.
        Assert.Throws<InvalidDataException>(() => CXEXParser.ParseSignature(image, 0xFFFF_FFFF));
        Assert.Throws<InvalidDataException>(() => CXEXParser.ParseSignature(image, (uint)image.Length));
    }
}
