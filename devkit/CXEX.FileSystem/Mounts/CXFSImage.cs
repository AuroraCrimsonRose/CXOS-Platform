using System;
using System.Collections.Generic;
using System.IO;
using CXEX.FileSystem.Volume;

namespace CXEX.FileSystem.Mounts;

public class CXFSImage : IDisposable
{
    private readonly Stream _diskStream;
    public CXFSSuperblock Superblock { get; private set; }
    public Dictionary<uint, CXFSEntry> Manifest { get; private set; } = new();

    public CXFSImage(Stream diskStream, ulong partitionBaseLba = 0)
    {
        _diskStream = diskStream;

        // Read Superblock (Sector 0 of the partition)
        _diskStream.Position = (long)partitionBaseLba * 512;
        byte[] block0 = new byte[4096];
        _diskStream.ReadExactly(block0, 0, 4096);

        Superblock = CXFSParser.ParseSuperblock(block0);

        HydrateManifest();
    }

    private void HydrateManifest()
    {
        byte[] manifestBuffer = new byte[Superblock.ManifestBlocks * Superblock.BlockSize];
        _diskStream.Position = (long)Superblock.ManifestStart * Superblock.BlockSize;
        _diskStream.ReadExactly(manifestBuffer, 0, manifestBuffer.Length);

        ReadOnlySpan<byte> span = manifestBuffer;
        int currentOffset = 0;

        for (uint i = 0; i < Superblock.ManifestCount; i++)
        {
            var entry = CXFSParser.ParseEntry(span, currentOffset);
            if (!entry.IsFree)
            {
                Manifest[entry.Id] = entry;
            }
            currentOffset += (int)Superblock.EntrySize;
        }
    }

    public byte[] ReadFileBytes(CXFSEntry fileEntry)
    {
        if (!fileEntry.IsFile) throw new InvalidOperationException("Entry is not a file.");
        if (fileEntry.Size == 0) return Array.Empty<byte>();

        byte[] output = new byte[fileEntry.Size];
        uint bytesRead = 0;

        for (int i = 0; i < 8 && bytesRead < fileEntry.Size; i++)
        {
            uint extStart = fileEntry.ExtentStart[i];
            uint extLen = fileEntry.ExtentLen[i];
            if (extLen == 0) continue;

            _diskStream.Position = (long)extStart * Superblock.BlockSize;

            uint chunkToRead = Math.Min((uint)(extLen * Superblock.BlockSize), (uint)fileEntry.Size - bytesRead);
            _diskStream.ReadExactly(output, (int)bytesRead, (int)chunkToRead);

            bytesRead += chunkToRead;
        }

        return output;
    }

    public void Dispose() => _diskStream?.Dispose();
}