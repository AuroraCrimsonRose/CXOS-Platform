// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using CXEX.Crypto.Trust;
using Xunit;
using CXEX.Tests;

namespace CXEX.Tests.Unit;

/// <summary>
/// One cryptographic profile, enforced in the crypto layer (DevKit security
/// review §6), and private signing keys handled as trust-root material rather
/// than as build output (§7).
///
/// <para>The §6 point is that policy belongs where the keys are made, not in
/// the front-end: <c>KeygenCommand</c> refused <c>--bits</c> other than 2048
/// and <c>CXKeyGenerator.Generate</c> accepted anything from 1024 up, so
/// Studio, a test, or any future command calling the library directly could
/// still mint a key CXK cannot verify. That key signs perfectly and every
/// artifact it produces is refused at boot as BAD_SIGNATURE - which reads as
/// tampering rather than as the wrong key size. It is the layering rule too
/// (engineering §8): the library is the authority.</para>
/// </summary>
public class CryptoPolicyTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("policy_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
        GC.SuppressFinalize(this);
    }

    private (string Sk, string Pk) Paths(string tag) =>
        (Path.Combine(_dir, $"{tag}.xksk"), Path.Combine(_dir, $"{tag}.xkpk"));

    [Theory]
    [InlineData(1024)]
    [InlineData(1536)]
    [InlineData(3072)]
    [InlineData(4096)]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_library_refuses_any_key_size_but_the_profile(int bits)
    {
        // Two independent checks enforce this and they overlap on every size:
        // the request filter on keyBits, and the fact-check on the modulus
        // length actually produced. Confirmed by sabotage - restoring the old
        // permissive `keyBits >= 1024` alone changed nothing here, because a
        // 1024-bit key still failed on its 128-byte modulus; only removing
        // both turned all four cases red. So this asserts the property, not
        // one line of it.
        var (sk, pk) = Paths($"b{bits}");
        Assert.Throws<CryptographicException>(() => CXKeyGenerator.Generate(sk, pk, bits));

        // And it refused before writing anything: a rejected request must not
        // leave key material behind.
        Assert.False(File.Exists(sk), "a refused key size still wrote a private key");
        Assert.False(File.Exists(pk), "a refused key size still wrote a public key");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_profile_itself_is_accepted_so_this_is_not_refusing_everything()
    {
        var (sk, pk) = Paths("good");
        byte[] xkpk = CXKeyGenerator.Generate(sk, pk, CXKeyGenerator.PROFILE_KEY_BITS);

        Assert.True(File.Exists(sk));
        Assert.True(File.Exists(pk));

        // 16-byte CXPK header plus a 256-byte modulus.
        Assert.Equal(16 + CXKeyGenerator.PROFILE_MODULUS_LEN, xkpk.Length);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_profile_constants_match_the_kernels_rsa_header()
    {
        // The DevKit declares the profile separately rather than sharing it,
        // so that CXK does not depend on the DevKit to decide what CXK can
        // verify (§12.5). Declared separately means they can drift, so the
        // kernel's header is read and compared.
        string? repo = TestEnv.RepoRoot;
        if (repo is null) { Assert.True(true, "no checkout: skipped"); return; }

        string rsaH = Path.Combine(repo, "kernel", "lib", "crypto", "rsa.h");
        if (!File.Exists(rsaH)) { Assert.True(true, "rsa.h not found: skipped"); return; }

        string text = File.ReadAllText(rsaH);
        Assert.Contains($"RSA_PROFILE_KEY_BITS    {CXKeyGenerator.PROFILE_KEY_BITS}u", text);
        Assert.Contains($"RSA_PROFILE_MODULUS_LEN {CXKeyGenerator.PROFILE_MODULUS_LEN}u", text);
        Assert.Contains($"RSA_PROFILE_EXPONENT    {CXKeyGenerator.PROFILE_EXPONENT}u", text);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_generated_key_uses_the_pinned_exponent()
    {
        var (sk, pk) = Paths("exp");
        byte[] xkpk = CXKeyGenerator.Generate(sk, pk);

        // exponent is a u32 at offset 8 of the CXPK header, LITTLE-endian
        // like every scalar in it - only the modulus that follows is
        // big-endian, because that is what the kernel's bn_from_bytes wants.
        // Worth being exact about: reading this one field the other way round
        // gives 0x01000100 for F4, which is a plausible-looking number.
        uint e = BitConverter.ToUInt32(xkpk, 8);
        Assert.Equal(CXKeyGenerator.PROFILE_EXPONENT, e);
    }

    // ---- §7: private keys are not ordinary build output ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_private_key_is_created_readable_only_by_its_owner()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            // Unix modes do not apply; the inherited directory ACL governs.
            // Said rather than silently passing on the platform where the
            // check is meaningless.
            Assert.True(true, "Windows: file mode not applicable");
            return;
        }

        var (sk, pk) = Paths("perm");
        CXKeyGenerator.Generate(sk, pk);

        UnixFileMode mode = File.GetUnixFileMode(sk);

        // Nobody but the owner, at all. File.WriteAllText used to create this
        // with the default mode, which umask normally leaves world-readable -
        // so a private signing key was readable by every user on the machine.
        Assert.Equal(UnixFileMode.None, mode & UnixFileMode.GroupRead);
        Assert.Equal(UnixFileMode.None, mode & UnixFileMode.GroupWrite);
        Assert.Equal(UnixFileMode.None, mode & UnixFileMode.OtherRead);
        Assert.Equal(UnixFileMode.None, mode & UnixFileMode.OtherWrite);
        Assert.Equal(UnixFileMode.UserRead, mode & UnixFileMode.UserRead);

        // The public half is public: asserting the private one is restricted
        // means nothing unless the two are actually treated differently.
        UnixFileMode pubMode = File.GetUnixFileMode(pk);
        Assert.NotEqual(mode, pubMode);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void No_private_key_material_appears_in_the_public_half()
    {
        var (sk, pk) = Paths("leak");
        CXKeyGenerator.Generate(sk, pk);

        byte[] xkpk = File.ReadAllBytes(pk);
        string pem = File.ReadAllText(sk);

        // The .xkpk is modulus + exponent and nothing else: 16 + 256 bytes
        // exactly, so there is no room for a private component even by
        // accident. Checked as a size rather than by searching for bytes,
        // because a size is the property that makes the leak impossible.
        Assert.Equal(16 + CXKeyGenerator.PROFILE_MODULUS_LEN, xkpk.Length);

        // And the private file really is a private key, so the comparison
        // above is not between two public halves.
        Assert.Contains("PRIVATE KEY", pem);
        Assert.DoesNotContain("PRIVATE KEY", System.Text.Encoding.ASCII.GetString(xkpk));
    }
}
