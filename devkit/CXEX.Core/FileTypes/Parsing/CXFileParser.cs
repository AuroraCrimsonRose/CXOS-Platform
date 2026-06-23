using System.Runtime.InteropServices;
using System; // Required for Slice (Span)

namespace CXEX.Core.FileTypes.Parsing;

public static class CXFileParser
{
    /// <summary>
    /// Parses a struct from the buffer at the specified offset.
    /// Uses MemoryMarshal for fast, padding-aware mapping (assuming layout was set in struct).
    /// </summary>
    public static T ParseStruct<T>(ReadOnlySpan<byte> data, int offset) where T : struct
    {
        if (data.Length < offset + Marshal.SizeOf<T>())
            throw new InvalidDataException($"Buffer too small to parse {typeof(T).Name}");

        return MemoryMarshal.Read<T>(data.Slice(offset));
    }

    /// <summary>
    /// Validates magic bytes at the start of the file.
    /// </summary>
    public static void ValidateMagic(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expected)
    {
        if (data.Length < expected.Length || !data.Slice(0, expected.Length).SequenceEqual(expected))
            throw new InvalidDataException("Invalid CXEX magic signature.");
    }
}