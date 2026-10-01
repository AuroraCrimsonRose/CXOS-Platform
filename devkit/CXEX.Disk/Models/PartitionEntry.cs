namespace CXEX.Disk.Models;

/// <summary>One partition / region within a disk image.</summary>
public sealed class PartitionEntry
{
    public int Index { get; init; }
    public string Name { get; init; } = "";
    public string TypeName { get; init; } = "";
    public string? TypeId { get; init; }       // MBR type byte (hex) or GPT type GUID
    public long StartLba { get; init; }
    public long SectorCount { get; init; }
    public long StartOffset { get; init; }      // byte offset into the image
    public long SizeBytes { get; init; }
    public bool Bootable { get; init; }
    public string? Guid { get; init; }          // GPT unique partition GUID

    public string SizeDisplay =>
        SizeBytes < 1024 ? $"{SizeBytes} B" :
        SizeBytes < 1024L * 1024 ? $"{SizeBytes / 1024.0:F1} KB" :
        SizeBytes < 1024L * 1024 * 1024 ? $"{SizeBytes / 1048576.0:F1} MB" :
        $"{SizeBytes / 1073741824.0:F2} GB";
}
