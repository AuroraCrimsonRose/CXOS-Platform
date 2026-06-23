using System;
using System.IO;
using System.Text;
using CXEX.Core.Utilities;
using CXEX.FileType.Structures;

namespace CXEX.FileType.Parsers;

public static class CXEXParser
{
    public const int HEADER_SIZE = 56;
    public const int SECTION_SIZE = 28;
    public const int SIG_HDR_SIZE = 42;

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

    public static CXEXSignatureBlock ParseSignature(ReadOnlySpan<byte> data, uint signatureOffset)
    {
        int offset = (int)signatureOffset;
        if (data.Length < offset + SIG_HDR_SIZE)
            throw new InvalidDataException("Signature block out of bounds.");

        uint magic = MemoryPrimitives.ReadU32(data, offset);
        if (magic != 0x47535843) // 'CXSG'
            throw new InvalidDataException("Invalid CXSG magic bytes.");

        var sig = new CXEXSignatureBlock
        {
            Magic = magic,
            SigAlgo = MemoryPrimitives.ReadU16(data, offset + 4),
            HashAlgo = MemoryPrimitives.ReadU16(data, offset + 6),
            SigLen = MemoryPrimitives.ReadU16(data, offset + 40),
            SigFileOffset = signatureOffset + SIG_HDR_SIZE
        };

        data.Slice(offset + 8, 32).CopyTo(sig.Fingerprint);

        if (data.Length < sig.SigFileOffset + sig.SigLen)
            throw new InvalidDataException("Signature payload out of bounds.");

        sig.Signature = data.Slice((int)sig.SigFileOffset, sig.SigLen).ToArray();
        return sig;
    }
}