using System.Text;

namespace CXEX.Core.Constants;

public static class CXMagic
{
    public static readonly uint CXEX = BinaryMagic("CXEX"); // Executables
    public static readonly uint CXPK = BinaryMagic("CXPK"); // Public Keys
    public static readonly uint CXSG = BinaryMagic("CXSG"); // Signature Blocks
    public static readonly uint XBPT = BinaryMagic("XBPT"); // Partition Tables
    public static readonly uint XSTG = BinaryMagic("XSTG"); // Stage Manifests

    private static uint BinaryMagic(string ascii)
    {
        byte[] bytes = Encoding.ASCII.GetBytes(ascii);
        return (uint)(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24));
    }
}