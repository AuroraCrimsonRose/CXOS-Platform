using System;
using System.IO;
using System.Text;
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

    public static CXPKHeader ParseCxpkHeader(ReadOnlySpan<byte> data)
    {
        if (data.Length < 16) throw new InvalidDataException("Data too small for CXPK header.");

        return new CXPKHeader
        {
            Magic = MemoryPrimitives.ReadU32(data, 0),
            Version = MemoryPrimitives.ReadU16(data, 4),
            KeyBits = MemoryPrimitives.ReadU16(data, 6),
            Exponent = MemoryPrimitives.ReadU32(data, 8),
            ModulusLen = MemoryPrimitives.ReadU16(data, 12)
        };
    }
}