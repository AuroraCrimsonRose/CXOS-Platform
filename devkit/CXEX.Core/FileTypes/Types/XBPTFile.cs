namespace CXEX.Core.FileTypes.Types;

public class XBPTFile : ICXFile
{
    public uint Version;
    public uint EntryCount;
    public List<XBPTEntry> Entries;

    public void Load(byte[] data) { /* Parse XBPT entries */ }
}