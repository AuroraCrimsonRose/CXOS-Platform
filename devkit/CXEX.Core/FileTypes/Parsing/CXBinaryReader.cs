using System;
using System.Buffers.Binary;

namespace CXEX.Core.FileTypes.Parsing;

public static class CXBinaryReader
{
    public static ushort ReadU16(ReadOnlySpan<byte> data, int offset)
        => BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(offset));

    public static uint ReadU32(ReadOnlySpan<byte> data, int offset)
        => BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(offset));

    public static ulong ReadU64(ReadOnlySpan<byte> data, int offset)
        => BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(offset));
}