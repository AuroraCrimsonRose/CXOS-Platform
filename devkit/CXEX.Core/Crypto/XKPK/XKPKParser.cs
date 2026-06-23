using CXEX.Core.FileTypes.Parsing;
using CXEX.Core.FileTypes.Types; // Required for Slice (Span)

namespace CXEX.Core.Crypto.XKPK;

public static class XKPKParser
{
    public static XKPKKey Parse(byte[] data)
    {
        var header = CXFileParser.ParseStruct<CXPKHeader>(data, 0);
        var modulus = data.Slice(16, header.ModulusLen).ToArray();
        return new XKPKKey { Header = header, Modulus = modulus };
    }
}