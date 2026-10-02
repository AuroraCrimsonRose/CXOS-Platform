// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;

namespace CXEX.FileType.Structures;

public class CXEXHeader
{
    public uint Magic { get; set; }
    public ushort TypeCode { get; set; }
    public ushort FormatVersion { get; set; }
    public ushort ArchTarget { get; set; }
    public ushort AbiVersion { get; set; }
    public uint Flags { get; set; }
    public uint EntryPoint { get; set; }
    public uint LoadBase { get; set; }
    public uint ImageMin { get; set; }
    public uint ImageMax { get; set; }
    public ushort SectionCount { get; set; }
    public ushort SectionOffset { get; set; }
    public uint RelocOffset { get; set; }
    public uint SignatureOffset { get; set; }
    public uint DependencyOffset { get; set; }

    public bool IsSigned => (Flags & 0x04) != 0 && SignatureOffset != 0;
}

public class CXEXSection
{
    public string Name { get; set; } = string.Empty;
    public uint FileOffset { get; set; }
    public uint VirtAddr { get; set; }
    public uint FileSize { get; set; }
    public uint MemSize { get; set; }
    public uint Flags { get; set; }
}

public class CXEXSignatureBlock
{
    public uint Magic { get; set; }
    public ushort SigAlgo { get; set; }
    public ushort HashAlgo { get; set; }
    /// <summary>sha256 of the .xkpk bytes carried at <see cref="PubKeyFileOffset"/>.</summary>
    public byte[] Fingerprint { get; set; } = new byte[32];

    /// <summary>Length of the signer's .xkpk, which travels with the image.</summary>
    public ushort PubKeyLen { get; set; }

    public ushort SigLen { get; set; }

    /// <summary>Where the signer's .xkpk starts: the signature block's own offset + 44.</summary>
    public uint PubKeyFileOffset { get; set; }

    /// <summary>Where the signature starts: <see cref="PubKeyFileOffset"/> + <see cref="PubKeyLen"/>.</summary>
    public uint SigFileOffset { get; set; }

    /// <summary>The signer's public key, verbatim, as the fingerprint is taken over it.</summary>
    public byte[] PubKey { get; set; } = Array.Empty<byte>();

    public byte[] Signature { get; set; } = Array.Empty<byte>();
}