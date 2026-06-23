using System;
using System.IO;
using System.Buffers.Binary;
using CXEX.Core.FileTypes; // Ensure this contains the CXPKHeader struct definition

namespace CXEX.Core.Crypto.XKPK;

public static class XKPKSerializer
{
    /// <summary>
    /// Serializes an XKPK key structure into binary format.
    /// Matches the exact memory layout of the CXPK file format.
    /// </summary>
    public static byte[] Serialize(CXPKHeader header, byte[] modulus)
    {
        // 16 bytes for header + N bytes for modulus
        int totalSize = 16 + modulus.Length;
        byte[] buffer = new byte[totalSize];
        Span<byte> span = buffer.AsSpan();

        // 1. Magic Bytes: 'CXPK'
        span[0] = (byte)'C';
        span[1] = (byte)'X';
        span[2] = (byte)'P';
        span[3] = (byte)'K';

        // 2. Header Fields (Little Endian)
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(4), header.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(6), header.KeyBits);
        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(8), header.Exponent);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(12), (ushort)modulus.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(14), header.Reserved);

        // 3. Modulus Payload
        modulus.CopyTo(span.Slice(16));

        return buffer;
    }

    /// <summary>
    /// Persists the key to a file system location.
    /// </summary>
    public static void SaveToFile(string path, CXPKHeader header, byte[] modulus)
    {
        byte[] data = Serialize(header, modulus);
        File.WriteAllBytes(path, data);
    }

    /// <summary>
    /// Convenience method for serializing an existing XKPKKey object.
    /// </summary>
    public static byte[] SerializeKey(XKPKKey key)
    {
        return Serialize(key.Header, key.Modulus);
    }
}