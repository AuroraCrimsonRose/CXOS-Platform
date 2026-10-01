using System;
using System.Buffers.Binary;

namespace CXEX.Core.Utilities;

public static class MemoryPrimitives
{
    public static ushort ReadU16(ReadOnlySpan<byte> data, int offset)
        => BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(offset));

    public static uint ReadU32(ReadOnlySpan<byte> data, int offset)
        => BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(offset));

    public static ulong ReadU64(ReadOnlySpan<byte> data, int offset)
        => BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(offset));

    public static void WriteU16(Span<byte> data, int offset, ushort value)
        => BinaryPrimitives.WriteUInt16LittleEndian(data.Slice(offset), value);

    public static void WriteU32(Span<byte> data, int offset, uint value)
        => BinaryPrimitives.WriteUInt32LittleEndian(data.Slice(offset), value);

    public static void WriteU64(Span<byte> data, int offset, ulong value)
        => BinaryPrimitives.WriteUInt64LittleEndian(data.Slice(offset), value);
}