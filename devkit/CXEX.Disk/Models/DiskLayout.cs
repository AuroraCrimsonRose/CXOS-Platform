using System.Collections.Generic;

namespace CXEX.Disk.Models;

/// <summary>Parsed view of a disk image: which table, and its partitions.</summary>
public sealed class DiskLayout
{
    public PartitionTableType TableType { get; init; } = PartitionTableType.Unknown;
    public int SectorSize { get; init; } = 512;
    public long DiskSizeBytes { get; init; }
    public string? DiskGuid { get; init; }
    public IReadOnlyList<PartitionEntry> Partitions { get; init; } = new List<PartitionEntry>();
    public string? Note { get; init; }
}
