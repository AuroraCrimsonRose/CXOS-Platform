namespace CXEX.Build.Layout;

// Represents the calculated footprint of a CXEX executable
public class CxexMemoryLayout
{
    public uint EntryPoint { get; set; }
    public uint PhysBase { get; set; } // Lowest LMA
    public uint ImageMin { get; set; } // Lowest VMA
    public uint ImageMax { get; set; } // Highest VMA + MemSize

    // Calculates where the section table ends and raw data begins
    public uint DataStartOffset => 56 + (uint)(Sections.Count * 28);

    public List<SectionLayout> Sections { get; set; } = new();
}

public class SectionLayout
{
    public string Name { get; set; } = string.Empty;
    public uint VirtualAddress { get; set; }
    public uint FileOffset { get; set; }
    public uint FileSize { get; set; }
    public uint MemSize { get; set; }
    public uint Flags { get; set; }

    // The raw extracted bytes from the ELF, held in memory until emission
    public byte[] Payload { get; set; } = Array.Empty<byte>();
}