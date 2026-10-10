// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.IO;
using System.Text;
using CXEX.Core.Constants;
using CXEX.Core.Utilities;
using CXEX.FileType.Structures;

namespace CXEX.FileType.Parsers;

public static class CXOtherParsers
{
    public static XBPTHeader ParseXbptHeader(ReadOnlySpan<byte> data)
    {
        if (data.Length < 32) throw new InvalidDataException("Data too small for XBPT header.");

        uint magic = MemoryPrimitives.ReadU32(data, 0);
        if (magic != 0x54504258) // 'XBPT' little-endian
            throw new InvalidDataException("Invalid XBPT magic signature.");

        return new XBPTHeader
        {
            Magic = magic,
            Version = MemoryPrimitives.ReadU16(data, 4),
            EntryCount = MemoryPrimitives.ReadU16(data, 6),
            EntrySize = MemoryPrimitives.ReadU16(data, 8),
            Flags = MemoryPrimitives.ReadU16(data, 10),
            TotalSectors = MemoryPrimitives.ReadU64(data, 12)
        };
    }

    public static XBPTEntry ParseXbptEntry(ReadOnlySpan<byte> data, int offset)
    {
        if (data.Length < offset + 32) throw new InvalidDataException("XBPT Entry out of bounds.");

        var nameSpan = data.Slice(offset + 20, 12);
        int nullIdx = nameSpan.IndexOf((byte)0);
        string name = Encoding.ASCII.GetString(nameSpan.Slice(0, nullIdx >= 0 ? nullIdx : 12));

        return new XBPTEntry
        {
            StartLba = MemoryPrimitives.ReadU64(data, offset),
            SectorCount = MemoryPrimitives.ReadU64(data, offset + 8),
            PartitionType = data[offset + 16],
            Flags = data[offset + 17],
            Name = name
        };
    }

    /// <summary>
    /// The largest modulus the kernel will accept: <c>rsa_verify_sha256</c> refuses
    /// anything over 256 bytes, and <c>rsa_parse_xkpk</c> bounds it by the fixed
    /// buffer it reads into. Matching the ceiling here means the DevKit cannot emit
    /// or accept a key the kernel would then refuse.
    /// </summary>
    private const int MaxModulusBytes = 256;   // RSA-2048

    // The one profile, mirroring kernel/lib/crypto/rsa.h. Declared in both places
    // deliberately: the kernel must not depend on the DevKit to decide what it
    // can verify (security review §12.5), so the two agree by being checked
    // against each other in tests rather than by sharing a definition.
    private const ushort SupportedVersion = 1;
    private const ushort ProfileKeyBits   = 2048;
    private const uint   ProfileExponent  = 65537;

    /// <summary>
    /// Reads and validates a CXPK public-key header.
    ///
    /// <para>This read the magic into the model and never compared it, so any file
    /// of at least 16 bytes parsed as a key - including an empty one, a PEM, or the
    /// private half by mistake. The fingerprint is sha256 over these exact bytes
    /// (CXKeyGenerator), so a garbage "key" still produces a confident-looking
    /// fingerprint and an image nobody can verify.</para>
    /// </summary>
    public static CXPKHeader ParseCxpkHeader(ReadOnlySpan<byte> data)
    {
        if (data.Length < 16) throw new InvalidDataException("Data too small for CXPK header.");

        uint magic = MemoryPrimitives.ReadU32(data, 0);
        if (magic != CXMagic.CXPK)
            throw new InvalidDataException("not a CXPK public key: missing the 'CXPK' magic.");

        var header = new CXPKHeader
        {
            Magic = magic,
            Version = MemoryPrimitives.ReadU16(data, 4),
            KeyBits = MemoryPrimitives.ReadU16(data, 6),
            Exponent = MemoryPrimitives.ReadU32(data, 8),
            ModulusLen = MemoryPrimitives.ReadU16(data, 12)
        };

        // The ONE profile CXK implements: RSA-2048 / SHA-256 / PKCS#1 v1.5
        // (security review §12.1). Checked here as well as in the kernel, because
        // the DevKit should refuse to produce what the kernel will refuse to load
        // - finding out at boot that a key is unusable is the worst time to find
        // out. A future profile gets a new CXSG algorithm identifier, never a
        // wider range accepted under this one.
        if (header.Version != SupportedVersion)
            throw new InvalidDataException(
                $"CXPK declares format version {header.Version}; this toolchain and CXK support {SupportedVersion}.");

        if (header.KeyBits != ProfileKeyBits)
            throw new InvalidDataException(
                $"CXPK declares a {header.KeyBits}-bit key; CXK implements RSA-{ProfileKeyBits} only.");

        if (header.Exponent != ProfileExponent)
            throw new InvalidDataException(
                $"CXPK public exponent is {header.Exponent}; the platform profile requires {ProfileExponent}.");

        if (header.ModulusLen == 0)
            throw new InvalidDataException("CXPK declares a zero-length modulus.");

        if (header.ModulusLen > MaxModulusBytes)
            throw new InvalidDataException(
                $"CXPK modulus is {header.ModulusLen} bytes, beyond the {MaxModulusBytes} the kernel accepts.");

        // key_bits is redundant with modulus_len, which is exactly why it is worth
        // checking: the two disagreeing means the file was built by something that
        // does not understand the format.
        if (header.KeyBits != header.ModulusLen * 8)
            throw new InvalidDataException(
                $"CXPK key_bits {header.KeyBits} disagrees with modulus_len {header.ModulusLen} ({header.ModulusLen * 8} bits).");

        // An RSA public exponent is odd and greater than 1. Zero or an even value is
        // not a key at all, and 1 would make every signature trivially forgeable.
        if (header.Exponent <= 1 || (header.Exponent & 1) == 0)
            throw new InvalidDataException($"CXPK public exponent {header.Exponent} is not a valid RSA exponent.");

        if (data.Length < 16 + header.ModulusLen)
            throw new InvalidDataException(
                $"CXPK declares a {header.ModulusLen}-byte modulus but the file holds {data.Length - 16} bytes after the header.");

        return header;
    }
}