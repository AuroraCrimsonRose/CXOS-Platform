namespace CXEX.Build.Layout;

// Represents the physical sector map of the target disk
public class DiskGeometryMap
{
    public ulong TotalSectors { get; set; }
    public uint SectorSize { get; set; } = 512;

    // Fixed Offsets
    public ulong Stage1Lba => 0;
    public ulong XbptTableLba => 1;
    public ulong Stage2Lba => 2;

    // Dynamic Partitions (Calculated during layout phase)
    public List<PartitionLayout> Partitions { get; set; } = new();
}

public class PartitionLayout
{
    public string Label { get; set; } = string.Empty;
    public byte TypeCode { get; set; }
    public byte Flags { get; set; }

    // The calculated geometry
    public ulong StartLba { get; set; }
    public ulong SectorCount { get; set; }

    // Helper to see where this partition physically ends
    public ulong EndLba => StartLba + SectorCount;
}