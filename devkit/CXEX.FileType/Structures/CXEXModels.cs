using System;

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
    public byte[] Fingerprint { get; set; } = new byte[32];
    public ushort SigLen { get; set; }
    public uint SigFileOffset { get; set; }
    public byte[] Signature { get; set; } = Array.Empty<byte>();
}