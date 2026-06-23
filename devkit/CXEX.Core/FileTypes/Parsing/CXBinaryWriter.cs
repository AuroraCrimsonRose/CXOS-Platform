using System;
using System.Buffers.Binary;

namespace CXEX.Core.FileTypes.Parsing;

public static class CXBinaryWriter
{
    public static void WriteU16(Span<byte> data, int offset, ushort value)
        => BinaryPrimitives.WriteUInt16LittleEndian(data.Slice(offset), value);

    public static void WriteU32(Span<byte> data, int offset, uint value)
        => BinaryPrimitives.WriteUInt32LittleEndian(data.Slice(offset), value);
}