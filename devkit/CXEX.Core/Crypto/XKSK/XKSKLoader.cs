using System.IO;

namespace CXEX.Core.Crypto.XKSK;

public static class XKSKLoader
{
    public static XKSKKey Load(string path)
    {
        byte[] data = File.ReadAllBytes(path);
        var (pub, privExp) = XKSKParser.Parse(data);
        return new XKSKKey(pub, privExp);
    }
}