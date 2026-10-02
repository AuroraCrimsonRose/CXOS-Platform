// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;
using System.IO;
using System.Text;
using CXEX.Core.Utilities;
using CXEX.FileType.Structures;

namespace CXEX.FileType.Parsers;

public static class CXEXParser
{
    public const int HEADER_SIZE = 56;
    public const int SECTION_SIZE = 28;
    public const int SIG_HDR_SIZE = 44;   // 4 magic + 2 + 2 + 32 fingerprint + 2 pubkey_len + 2 sig_len

    public static CXEXHeader ParseHeader(ReadOnlySpan<byte> data)
    {
        if (data.Length < HEADER_SIZE)
            throw new InvalidDataException("Data too small to contain a CXEX header.");

        uint magic = MemoryPrimitives.ReadU32(data, 0);
        if (magic != 0x58455843) // 'CXEX' in little-endian
            throw new InvalidDataException("Invalid CXEX magic bytes.");

        return new CXEXHeader
        {
            Magic = magic,
            TypeCode = MemoryPrimitives.ReadU16(data, 4),
            FormatVersion = MemoryPrimitives.ReadU16(data, 6),
            ArchTarget = MemoryPrimitives.ReadU16(data, 8),
            AbiVersion = MemoryPrimitives.ReadU16(data, 10),
            Flags = MemoryPrimitives.ReadU32(data, 12),
            EntryPoint = MemoryPrimitives.ReadU32(data, 16),
            LoadBase = MemoryPrimitives.ReadU32(data, 20),
            ImageMin = MemoryPrimitives.ReadU32(data, 24),
            ImageMax = MemoryPrimitives.ReadU32(data, 28),
            SectionCount = MemoryPrimitives.ReadU16(data, 32),
            SectionOffset = MemoryPrimitives.ReadU16(data, 34),
            RelocOffset = MemoryPrimitives.ReadU32(data, 36),
            SignatureOffset = MemoryPrimitives.ReadU32(data, 40),
            DependencyOffset = MemoryPrimitives.ReadU32(data, 44)
        };
    }

    public static CXEXSection ParseSection(ReadOnlySpan<byte> data, int offset)
    {
        if (data.Length < offset + SECTION_SIZE)
            throw new InvalidDataException("Section table bounds exceeded.");

        // Read 8-byte name and trim nulls
        var nameSpan = data.Slice(offset, 8);
        int nullIdx = nameSpan.IndexOf((byte)0);
        int nameLen = nullIdx >= 0 ? nullIdx : 8;
        string name = Encoding.ASCII.GetString(nameSpan.Slice(0, nameLen));

        return new CXEXSection
        {
            Name = name,
            FileOffset = MemoryPrimitives.ReadU32(data, offset + 8),
            VirtAddr = MemoryPrimitives.ReadU32(data, offset + 12),
            FileSize = MemoryPrimitives.ReadU32(data, offset + 16),
            MemSize = MemoryPrimitives.ReadU32(data, offset + 20),
            Flags = MemoryPrimitives.ReadU32(data, offset + 24)
        };
    }

    /// <summary>
    /// Reads a CXSG block. The layout is the kernel's <c>struct cxex_sig</c>
    /// (kernel/lib/format/cxex.h), which CXSigner writes and cxex_verify.c reads:
    ///
    /// <code>
    ///    0  "CXSG"        4
    ///    4  sig_algo      2
    ///    6  hash_algo     2
    ///    8  fingerprint  32   sha256 of the .xkpk carried below
    ///   40  pubkey_len    2
    ///   42  sig_len       2
    ///   44  pubkey     pubkey_len   the signer's .xkpk, verbatim
    ///      signature   sig_len
    /// </code>
    ///
    /// <para>This read the header as 42 bytes, took <c>sig_len</c> from offset 40 -
    /// which is <c>pubkey_len</c> - and placed the signature at <c>offset + 42</c>,
    /// which is where <c>sig_len</c> itself lives. On a real signed image it
    /// therefore reported a 272-byte signature (the .xkpk's length) where RSA-2048
    /// produces exactly 256, and handed CXVerifier the length field plus the first
    /// 270 bytes of the public key instead of the signature. Every verification of
    /// a genuinely signed image was being done over the wrong bytes.</para>
    /// </summary>
    public static CXEXSignatureBlock ParseSignature(ReadOnlySpan<byte> data, uint signatureOffset)
    {
        // Compared as long throughout. signatureOffset is unsigned and attacker
        // supplied: cast to int first, 0xFFFFFFFF becomes -1, and a
        // "data.Length < offset + 42" test then passes for any file over 41 bytes
        // before indexing at -1.
        if ((long)signatureOffset + SIG_HDR_SIZE > data.Length)
            throw new InvalidDataException(
                $"CXSG block at {signatureOffset} does not fit in a {data.Length}-byte file.");

        int offset = (int)signatureOffset;

        uint magic = MemoryPrimitives.ReadU32(data, offset);
        if (magic != 0x47535843) // 'CXSG'
            throw new InvalidDataException("Invalid CXSG magic bytes.");

        var sig = new CXEXSignatureBlock
        {
            Magic = magic,
            SigAlgo = MemoryPrimitives.ReadU16(data, offset + 4),
            HashAlgo = MemoryPrimitives.ReadU16(data, offset + 6),
            PubKeyLen = MemoryPrimitives.ReadU16(data, offset + 40),
            SigLen = MemoryPrimitives.ReadU16(data, offset + 42),
        };

        data.Slice(offset + 8, 32).CopyTo(sig.Fingerprint);

        sig.PubKeyFileOffset = signatureOffset + SIG_HDR_SIZE;
        sig.SigFileOffset = (uint)((long)sig.PubKeyFileOffset + sig.PubKeyLen);

        if ((long)sig.PubKeyFileOffset + sig.PubKeyLen > data.Length)
            throw new InvalidDataException(
                $"CXSG public key ({sig.PubKeyLen} bytes at {sig.PubKeyFileOffset}) runs past the end of the file.");

        if ((long)sig.SigFileOffset + sig.SigLen > data.Length)
            throw new InvalidDataException(
                $"CXSG signature ({sig.SigLen} bytes at {sig.SigFileOffset}) runs past the end of the file.");

        sig.PubKey = data.Slice((int)sig.PubKeyFileOffset, sig.PubKeyLen).ToArray();
        sig.Signature = data.Slice((int)sig.SigFileOffset, sig.SigLen).ToArray();
        return sig;
    }
}