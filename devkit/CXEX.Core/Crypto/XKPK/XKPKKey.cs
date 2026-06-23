using System.IO;
using System.Buffers.Binary;
using System.Security.Cryptography;
using CXEX.Core.FileTypes; // For CXPKHeader
using CXEX.Core.FileTypes.Parsing;

namespace CXEX.Core.Crypto.XKPK;

public class XKPKKey : ICXPublicKey
{
    public CXPKHeader Header { get; private set; }
    public byte[] Modulus { get; private set; }

    // ICXPublicKey Interface Implementation
    public CXKeyType Algorithm => CXKeyType.RSA2048_SHA256;
    public CXKeyRole Role => CXKeyRole.Kernel;

    public XKPKKey(CXPKHeader header, byte[] modulus)
    {
        Header = header;
        Modulus = modulus;
    }

    /// <summary>
    /// Serializes this key instance into the binary format.
    /// </summary>
    public byte[] ToBytes()
    {
        int totalSize = 16 + Modulus.Length;
        byte[] buffer = new byte[totalSize];
        Span<byte> span = buffer.AsSpan();

        // Magic
        span[0] = (byte)'C'; span[1] = (byte)'X'; span[2] = (byte)'P'; span[3] = (byte)'K';

        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(4), Header.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(6), Header.KeyBits);
        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(8), Header.Exponent);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(12), (ushort)Modulus.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(14), Header.Reserved);

        Modulus.CopyTo(span.Slice(16));
        return buffer;
    }

    /// <summary>
    /// Static factory to parse a key from raw bytes.
    /// </summary>
    public static XKPKKey Parse(byte[] data)
    {
        // Using our CXFileParser utility defined earlier
        var header = CXFileParser.ParseStruct<CXPKHeader>(data, 0);
        var modulus = data.AsSpan(16, header.ModulusLen).ToArray();
        return new XKPKKey(header, modulus);
    }

    public void SaveToFile(string path) => File.WriteAllBytes(path, ToBytes());

    public byte[] GetFingerprint() => SHA256.HashData(Modulus);

    public bool Verify(byte[] data, byte[] signature)
    {
        // Implement RSA verification using Modulus and Exponent
        // This keeps the verification logic local to the key definition
        return true;
    }
}