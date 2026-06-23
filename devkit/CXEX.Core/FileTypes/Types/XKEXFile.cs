namespace CXEX.Core.FileTypes.Types;

public class XKEXFile : ICXFile
{
    public CXEXHeader Header; // struct CXEXHeader
    public List<CXEXSection> Sections;

    public void Load(byte[] data) { /* Parse Header + Section Table */ }
}