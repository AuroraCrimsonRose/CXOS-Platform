// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Build.Emitters;
using CXEX.Build.Engines;
using CXEX.Build.Parsers;
using CXEX.Core.Constants;
using CXEX.Crypto.Signing;
using CXEX.Crypto.Trust;
using CXEX.Tests.Adversarial;
using CXEX.Tests.Differential;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// Identical inputs produce identical CXEX bytes (CX_EXTENSION_SYSTEM.md §9.9;
/// security review §12, engineering review §11).
///
/// <para>For signed software this is a security property, not tidiness: it is
/// what lets anyone rebuild an image from source and compare hashes, rather than
/// take the signer's word for what was signed. Until these existed nothing had
/// ever checked it. It happened to hold - two clean builds, and a third from a
/// different build directory, produced the same 11 artifacts byte for byte - but
/// a property that holds by accident is one the next change can take away
/// without anyone noticing.</para>
///
/// <para>Each layer is tested on its own, so a failure names the layer that
/// broke: packaging, signing, and the compiler end to end.</para>
/// </summary>
public class DeterminismTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("determ_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
        GC.SuppressFinalize(this);
    }

    /// <summary>
    /// Text, read-only data, and data with a .bss tail - one of each section kind
    /// the layout engine names, so ordering and NOBITS handling are both exercised.
    /// </summary>
    private static byte[] ThreeSegmentElf()
    {
        static byte[] Fill(int n, int seed) => Enumerable.Range(0, n).Select(i => (byte)(seed + i * 7)).ToArray();

        var b = Elf32Builder.Valid(96);
        b.Segments.Add(new Elf32Builder.Phdr
        {
            Offset = 0x2000, Vaddr = 0x00402000, Paddr = 0x00402000,
            FileSize = 48, MemSize = 48, Flags = 0x4, Align = 0x1000, Payload = Fill(48, 0x11),
        });
        b.Segments.Add(new Elf32Builder.Phdr
        {
            Offset = 0x3000, Vaddr = 0x00403000, Paddr = 0x00403000,
            FileSize = 32, MemSize = 0x200, Flags = 0x6, Align = 0x1000, Payload = Fill(32, 0x40),
        });
        return b.Build();
    }

    private static void Package(byte[] elf, string outPath)
    {
        var (entry, segments) = ElfParser.Parse(elf);
        CXEXWriter.WriteExecutable(outPath, CXEXLayoutEngine.CreateLayout(entry, segments, CXFlags.TYPE_USER));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Packaging_the_same_elf_twice_yields_identical_bytes()
    {
        // Different directories and different names: where an image is written
        // must not be something the image records.
        byte[] elf = ThreeSegmentElf();
        string a = Path.Combine(Directory.CreateDirectory(Path.Combine(_dir, "a")).FullName, "prog.xuex");
        string b = Path.Combine(Directory.CreateDirectory(Path.Combine(_dir, "b", "deeper")).FullName, "other.xuex");

        Package(elf, a);
        Package(elf, b);

        Assert.Equal(File.ReadAllBytes(a), File.ReadAllBytes(b));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Signing_the_same_image_twice_with_one_key_yields_identical_bytes()
    {
        // This holds because the signature is RSA PKCS#1 v1.5, which has no random
        // input (CX_EXTENSION_SYSTEM.md §10.2). PSS or ECDSA would draw fresh
        // randomness per signature. If that change is ever made deliberately, this
        // test is the record that the invariant narrows to the unsigned image -
        // change §9.9 with it, do not just delete the assertion.
        string sk = Path.Combine(_dir, "k.xksk");
        string pk = Path.Combine(_dir, "k.xkpk");
        CXKeyGenerator.Generate(sk, pk);

        byte[] elf = ThreeSegmentElf();
        string a = Path.Combine(_dir, "a.xuex");
        string b = Path.Combine(_dir, "b.xuex");
        Package(elf, a);
        Package(elf, b);

        CXSigner.SignArtifact(a, sk, pk);
        CXSigner.SignArtifact(b, sk, pk);

        Assert.Equal(File.ReadAllBytes(a), File.ReadAllBytes(b));
    }

    /// <summary>
    /// The whole chain: X source, through the C# compiler, clang and ld.lld, to a
    /// packaged image. Compiles <c>xc.xfxn</c> - the X compiler, the largest X
    /// program in the tree and so the one with the most symbols, strings and
    /// tables for an unordered collection to scramble.
    ///
    /// <para>The assembly is compared as well as the image, and first. Comments
    /// and symbol names never reach a CXEX, so nondeterminism there would leave
    /// the images equal and still be a defect waiting for the day it reaches
    /// emitted data; the .s is where it shows up first.</para>
    ///
    /// <para>Needs clang and ld.lld but not a Linux host: nothing is executed.</para>
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Toolchain)]
    public void Compiling_the_same_x_twice_from_different_directories_yields_identical_bytes()
    {
        string cxk = Requires.Cxk();
        string repo = Requires.Repo();
        Requires.Toolchain();

        string os = Path.Combine(repo, "os");
        string src = Path.Combine(os, "xc", "xc.xfxn");

        (byte[] Asm, byte[] Image) Build(string workDir)
        {
            Directory.CreateDirectory(workDir);
            string elf = Path.Combine(workDir, "xc.elf");
            string image = Path.Combine(workDir, "xc.xuex");

            var c = HostBuild.Run(cxk, new[]
            {
                "compile", src, elf, "--emit-asm",
                "-I", Path.Combine(os, "xc", "cxk"),
                "-I", Path.Combine(os, "xc"),
                "-I", Path.Combine(os, "std"),
            }, workDir);
            Assert.True(c.ExitCode == 0, $"cxk compile failed in {workDir}:\n{c.All}");

            var p = HostBuild.Run(cxk, new[] { "build", elf, image, "--type", "user" }, workDir);
            Assert.True(p.ExitCode == 0, $"cxk build failed in {workDir}:\n{p.All}");

            return (File.ReadAllBytes(Path.ChangeExtension(elf, ".s")), File.ReadAllBytes(image));
        }

        var first = Build(Path.Combine(_dir, "one"));
        var second = Build(Path.Combine(_dir, "two", "nested", "deeper"));

        Assert.Equal(first.Asm, second.Asm);
        Assert.Equal(first.Image, second.Image);
    }
}
