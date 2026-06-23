namespace CXEX.Core.Crypto;

public enum CXKeyRole : ushort
{
    Kernel = 0x01,
    Boot = 0x02,
    User = 0x03,
    Root = 0xFF
}