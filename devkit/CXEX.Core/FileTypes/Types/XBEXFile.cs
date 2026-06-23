namespace CXEX.Core.FileTypes.Types;

public class XOEXFile : ICXFile
{
    public CXEXHeader Header;
    public List<CXEXSection> Sections;

    public void Load(byte[] data) { /* Same parsing as XKEX, but with 'os' type-code checking */ }
}