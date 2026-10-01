using System;
using System.IO;
using CXEX.Core.Interfaces;
using CXEX.FileType.Structures;
using CXEX.FileType.Parsers;

namespace CXEX.FileType.Types;

public class XKPKFile : ICXFile
{
    public CXPKHeader Header { get; private set; } = null!;
    public byte[] Modulus { get; private set; } = Array.Empty<byte>();

    public string GetDisplayName() => "XKPK Cryptographic Public Key";

    public void Load(byte[] data)
    {
        ReadOnlySpan<byte> span = data;
        Header = CXOtherParsers.ParseCxpkHeader(span);

        if (span.Length < 16 + Header.ModulusLen)
            throw new InvalidDataException("Modulus stream truncated or invalid.");

        Modulus = span.Slice(16, Header.ModulusLen).ToArray();
    }
}