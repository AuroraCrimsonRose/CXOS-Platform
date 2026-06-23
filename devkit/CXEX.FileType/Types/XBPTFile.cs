using System;
using System.Collections.Generic;
using CXEX.Core.Interfaces;
using CXEX.FileType.Structures;
using CXEX.FileType.Parsers;

namespace CXEX.FileType.Types;

public class XBPTFile : ICXFile
{
    public XBPTHeader Header { get; private set; } = null!;
    public List<XBPTEntry> Partitions { get; private set; } = new();

    public string GetDisplayName() => "XBPT System Partition Table";

    public void Load(byte[] data)
    {
        ReadOnlySpan<byte> span = data;
        Header = CXOtherParsers.ParseXbptHeader(span);

        int offset = 32; // Header size from mkdisk.py
        for (int i = 0; i < Header.EntryCount; i++)
        {
            Partitions.Add(CXOtherParsers.ParseXbptEntry(span, offset));
            offset += Header.EntrySize;
        }
    }
}