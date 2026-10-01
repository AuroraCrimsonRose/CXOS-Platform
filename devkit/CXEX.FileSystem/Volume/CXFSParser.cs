using System;
using System.IO;
using System.Text;
using CXEX.Core.Utilities;

namespace CXEX.FileSystem.Volume;

public static class CXFSParser
{
    public static CXFSSuperblock ParseSuperblock(ReadOnlySpan<byte> data)
    {
        if (data.Length < 76) throw new InvalidDataException("Data too small for CXFS Superblock.");

        uint magic = MemoryPrimitives.ReadU32(data, 0);
        if (magic != 0x43584653) // "CXFS"
            throw new InvalidDataException("Invalid CXFS magic signature.");

        return new CXFSSuperblock
        {
            Magic = magic,
            Version = MemoryPrimitives.ReadU16(data, 4),
            BlockSize = MemoryPrimitives.ReadU16(data, 6),
            BaseLba = MemoryPrimitives.ReadU64(data, 8),
            TotalBlocks = MemoryPrimitives.ReadU32(data, 16),
            BitmapStart = MemoryPrimitives.ReadU32(data, 20),
            BitmapBlocks = MemoryPrimitives.ReadU32(data, 24),
            ManifestStart = MemoryPrimitives.ReadU32(data, 28),
            ManifestBlocks = MemoryPrimitives.ReadU32(data, 32),
            ManifestCount = MemoryPrimitives.ReadU32(data, 36),
            DataStart = MemoryPrimitives.ReadU32(data, 40),
            ReservedBlocks = MemoryPrimitives.ReadU32(data, 44),
            RootId = MemoryPrimitives.ReadU32(data, 48),
            FeatureFlags = MemoryPrimitives.ReadU32(data, 52),
            Created = MemoryPrimitives.ReadU64(data, 56),
            Modified = MemoryPrimitives.ReadU64(data, 64),
            EntrySize = MemoryPrimitives.ReadU32(data, 72)
        };
    }

    public static CXFSEntry ParseEntry(ReadOnlySpan<byte> data, int offset)
    {
        if (data.Length < offset + 188) throw new InvalidDataException("CXFS Entry out of bounds.");

        var entry = new CXFSEntry
        {
            Id = MemoryPrimitives.ReadU32(data, offset + 0),
            ParentId = MemoryPrimitives.ReadU32(data, offset + 4),
            Type = data[offset + 8],
            Flags = data[offset + 9],
            NameLen = data[offset + 10]
        };

        // Extract 64-byte Name
        var nameSpan = data.Slice(offset + 12, 64);
        int nullIdx = nameSpan.IndexOf((byte)0);
        entry.Name = Encoding.ASCII.GetString(nameSpan.Slice(0, nullIdx >= 0 ? nullIdx : 64));

        entry.Size = MemoryPrimitives.ReadU64(data, offset + 76);

        // Extract 8 Extents
        for (int i = 0; i < 8; i++)
        {
            entry.ExtentStart[i] = MemoryPrimitives.ReadU32(data, offset + 84 + (i * 4));
            entry.ExtentLen[i] = MemoryPrimitives.ReadU32(data, offset + 116 + (i * 4));
        }

        entry.OwnerUid = MemoryPrimitives.ReadU32(data, offset + 148);
        entry.GroupId = MemoryPrimitives.ReadU32(data, offset + 152);
        entry.Permissions = MemoryPrimitives.ReadU16(data, offset + 156);
        entry.LockState = data[offset + 158];
        entry.LockOwnerPid = MemoryPrimitives.ReadU32(data, offset + 160);
        entry.Created = MemoryPrimitives.ReadU64(data, offset + 164);
        entry.Modified = MemoryPrimitives.ReadU64(data, offset + 172);
        entry.Accessed = MemoryPrimitives.ReadU64(data, offset + 180);

        return entry;
    }
}