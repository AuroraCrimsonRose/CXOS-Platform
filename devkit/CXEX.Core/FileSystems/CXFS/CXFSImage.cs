using System;
using System.IO;
using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Collections.Generic;

namespace CXEX.Core.FileSystems.CXFS;

public class CXFSImage : IDisposable
{
    private readonly Stream _stream;
    private readonly bool _leaveOpen;

    public long BaseLba { get; private set; }
    public CxfsSuperblock Superblock { get; private set; }
    public Dictionary<uint, CxfsManifestEntry> Manifest { get; } = new();

    public CXFSImage(Stream stream, bool leaveOpen = false)
    {
        _stream = stream ?? throw new ArgumentNullException(nameof(stream));
        _leaveOpen = leaveOpen;
        InitializeVolume();
    }

    private void InitializeVolume()
    {
        BaseLba = DiscoverBaseLba();

        // Read Superblock (Block 0)
        byte[] sbBuffer = new byte[CXFSConstants.BlockSize];
        _stream.Position = BaseLba * CXFSConstants.SectorSize;
        int read = _stream.Read(sbBuffer, 0, sbBuffer.Length);
        if (read < sbBuffer.Length)
            throw new EndOfStreamException("Unable to read full CXFS superblock.");

        Superblock = MemoryMarshal.Read<CxfsSuperblock>(sbBuffer);

        if (Superblock.Magic != CXFSConstants.CxfsMagic)
            throw new InvalidDataException($"Invalid CXFS Magic number: Expected 0x{CXFSConstants.CxfsMagic:X8}, Found 0x{Superblock.Magic:X8}");

        if (Superblock.Version != CXFSConstants.ExpectedVersion)
            throw new NotSupportedException($"Unsupported CXFS Version: Volume version is {Superblock.Version}. Parser targets v{CXFSConstants.ExpectedVersion}.");

        LoadManifest();
    }

    private long DiscoverBaseLba()
    {
        if (_stream.Length < CXFSConstants.SectorSize * 2)
            return 0; // Fallback to raw disk image handling

        byte[] sectorBuffer = new byte[CXFSConstants.SectorSize];
        _stream.Position = CXFSConstants.SectorSize; // LBA 1
        _stream.ReadExactly(sectorBuffer, 0, (int)CXFSConstants.SectorSize);

        XbptHeader header = MemoryMarshal.Read<XbptHeader>(sectorBuffer);
        if (header.Magic != CXFSConstants.XbptMagic)
            return 0; // Zero out layout offset: Handed a raw filesystem dump

        int entryOffset = Marshal.SizeOf<XbptHeader>();
        int entrySize = header.EntrySize == 0 ? Marshal.SizeOf<XbptPartitionEntry>() : header.EntrySize;

        for (int i = 0; i < header.EntryCount; i++)
        {
            ReadOnlySpan<byte> entrySpan = sectorBuffer.AsSpan(entryOffset + (i * entrySize), entrySize);
            XbptPartitionEntry entry = MemoryMarshal.Read<XbptPartitionEntry>(entrySpan);

            if (entry.Type == CXFSConstants.PartitionTypeCxfs)
                return (long)entry.StartLba;
        }

        return 0;
    }

    private void LoadManifest()
    {
        Manifest.Clear();
        uint entriesPerBlock = Superblock.BlockSize / Superblock.EntrySize;
        byte[] blockBuffer = new byte[Superblock.BlockSize];

        for (uint b = 0; b < Superblock.ManifestBlocks; b++)
        {
            long blockNum = Superblock.ManifestStart + b;
            long offset = CalculateByteOffset(blockNum);

            _stream.Position = offset;
            _stream.ReadExactly(blockBuffer, 0, blockBuffer.Length);

            for (int entryIdx = 0; entryIdx < entriesPerBlock; entryIdx++)
            {
                int itemOffset = (int)(entryIdx * Superblock.EntrySize);
                ReadOnlySpan<byte> entrySpan = blockBuffer.AsSpan(itemOffset, (int)Superblock.EntrySize);
                CxfsManifestEntry entry = MemoryMarshal.Read<CxfsManifestEntry>(entrySpan);

                if (entry.Type != CXFSConstants.TypeFree)
                {
                    Manifest[entry.Id] = entry;
                }
            }
        }
    }

    public long CalculateByteOffset(long blockNumber)
    {
        return (BaseLba * (long)CXFSConstants.SectorSize) + (blockNumber * (long)Superblock.BlockSize);
    }

    public Stream CreateFileStream(CxfsManifestEntry entry)
    {
        if (entry.Type != CXFSConstants.TypeFile)
            throw new InvalidOperationException("Cannot establish file bounds stream on directory types.");

        return new CXFSBoundedBlockStream(this, entry);
    }

    public void Dispose()
    {
        if (!_leaveOpen) _stream.Dispose();
    }
}