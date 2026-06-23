namespace CXEX.FileType.Structures;

public class XBPTHeader
{
    public uint Magic { get; set; }        // 4 bytes: 'XBPT'
    public ushort Version { get; set; }    // 2 bytes
    public ushort EntryCount { get; set; } // 2 bytes
    public ushort EntrySize { get; set; }  // 2 bytes
    public ushort Flags { get; set; }      // 2 bytes
    public ulong TotalSectors { get; set; } // 8 bytes
}

public class XBPTEntry
{
    public ulong StartLba { get; set; }    // 8 bytes
    public ulong SectorCount { get; set; } // 8 bytes
    public byte PartitionType { get; set; } // 1 byte
    public byte Flags { get; set; }        // 1 byte
    public string Name { get; set; } = string.Empty; // 12 bytes
}

public class CXPKHeader
{
    public uint Magic { get; set; }        // 4 bytes: 'CXPK'
    public ushort Version { get; set; }    // 2 bytes
    public ushort KeyBits { get; set; }    // 2 bytes
    public uint Exponent { get; set; }     // 4 bytes
    public ushort ModulusLen { get; set; } // 2 bytes
}