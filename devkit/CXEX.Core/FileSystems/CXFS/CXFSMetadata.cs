using System;
using System.Runtime.InteropServices;

namespace CXEX.Core.FileSystems.CXFS;

public static class CXFSConstants
{
    public const uint XbptMagic = 0x54504258; // ASCII 'X' 'B' 'P' 'T'
    public const uint CxfsMagic = 0x43584653; // 'CSFX' in Little Endian
    public const ushort ExpectedVersion = 2;
    public const uint BlockSize = 4096;
    public const uint SectorSize = 512;
    public const byte PartitionTypeCxfs = 0xC5;

    public const byte TypeFree = 0;
    public const byte TypeFile = 1;
    public const byte TypeDir = 2;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct XbptHeader
{
    public uint Magic;
    public ushort Version;
    public ushort EntryCount;
    public ushort EntrySize;
    public ushort Flags;
    public ulong DiskSectors;
    public fixed byte Reserved[12];
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct XbptPartitionEntry
{
    public ulong StartLba;
    public ulong Sectors;
    public byte Type;
    public byte Flags;
    public fixed byte Reserved[2];
    public fixed byte Name[12];
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct CxfsSuperblock
{
    public uint Magic;
    public ushort Version;
    public ushort BlockSize;
    public ulong BaseLba;
    public uint TotalBlocks;
    public uint BitmapStart;
    public uint BitmapBlocks;
    public uint ManifestStart;
    public uint ManifestBlocks;
    public uint ManifestCount;
    public uint DataStart;
    public uint ReservedBlocks;
    public uint RootId;
    public uint FeatureFlags;
    public ulong Created;
    public ulong Modified;
    public uint EntrySize;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct CxfsManifestEntry
{
    public uint Id;
    public uint ParentId;
    public byte Type;
    public byte Flags;
    public byte NameLen;
    public byte Reserved0;
    public fixed byte Name[64];
    public ulong Size;
    public fixed uint ExtentStart[8];
    public fixed uint ExtentLen[8];
    public uint OwnerUid;
    public uint GroupId;
    public ushort Permissions;
    public byte LockState;
    public byte LockPad;
    public uint LockOwnerPid;
    public ulong Created;
    public ulong Modified;
    public ulong Accessed;
    public fixed byte Pad1[68];
}