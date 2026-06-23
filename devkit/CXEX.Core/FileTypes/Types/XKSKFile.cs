namespace CXEX.Core.FileTypes.Types;

public class XKSKFile : ICXFile
{
    public CXPKHeader Header;
    public byte[] Modulus;
    public byte[] PrivateExponent;

    public void Load(byte[] data) { /* Parse header, modulus, then private components */ }
}