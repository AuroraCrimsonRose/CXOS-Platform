using CXEX.Core.Crypto.XKPK;
using System; // Required for Slice (Span)

namespace CXEX.Core.Crypto.XKSK;

public static class XKSKParser
{
    public static (XKPKKey pub, byte[] privateExp) Parse(byte[] data)
    {
        // Header (16) + Modulus (N) + PrivateExponent (N)
        var pub = XKPKParser.Parse(data);
        var offset = 16 + pub.Header.ModulusLen;
        var privExp = data.Slice(offset, pub.Header.ModulusLen).ToArray();
        return (pub, privExp);
    }
}