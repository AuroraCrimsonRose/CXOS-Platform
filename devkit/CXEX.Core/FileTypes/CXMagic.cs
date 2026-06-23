namespace CXEX.Core.FileTypes;

public static class CXMagic
{
    public static ReadOnlySpan<byte> CXEX => new byte[] { (byte)'C', (byte)'X', (byte)'E', (byte)'X' };
    public static ReadOnlySpan<byte> CXPK => new byte[] { (byte)'C', (byte)'X', (byte)'P', (byte)'K' };
    public static ReadOnlySpan<byte> XBPT => new byte[] { (byte)'X', (byte)'B', (byte)'P', (byte)'T' };
}