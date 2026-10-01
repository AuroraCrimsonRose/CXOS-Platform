using System;

namespace CXEX.FileSystem.Volume;

public class CXFSSuperblock
{
    public uint Magic { get; set; }
    public ushort Version { get; set; }
    public ushort BlockSize { get; set; }
    public ulong BaseLba { get; set; }
    public uint TotalBlocks { get; set; }
    public uint BitmapStart { get; set; }
    public uint BitmapBlocks { get; set; }
    public uint ManifestStart { get; set; }
    public uint ManifestBlocks { get; set; }
    public uint ManifestCount { get; set; }
    public uint DataStart { get; set; }
    public uint ReservedBlocks { get; set; }
    public uint RootId { get; set; }
    public uint FeatureFlags { get; set; }
    public ulong Created { get; set; }
    public ulong Modified { get; set; }
    public uint EntrySize { get; set; }
}

public class CXFSEntry
{
    public uint Id { get; set; }
    public uint ParentId { get; set; }
    public byte Type { get; set; }
    public byte Flags { get; set; }
    public byte NameLen { get; set; }
    public string Name { get; set; } = string.Empty;
    public ulong Size { get; set; }
    public uint[] ExtentStart { get; set; } = new uint[8];
    public uint[] ExtentLen { get; set; } = new uint[8];
    public uint OwnerUid { get; set; }
    public uint GroupId { get; set; }
    public ushort Permissions { get; set; }
    public byte LockState { get; set; }
    public uint LockOwnerPid { get; set; }
    public ulong Created { get; set; }
    public ulong Modified { get; set; }
    public ulong Accessed { get; set; }

    public bool IsDirectory => Type == 2; // CXFS_TYPE_DIR
    public bool IsFile => Type == 1;      // CXFS_TYPE_FILE
    public bool IsFree => Type == 0;      // CXFS_TYPE_FREE
}