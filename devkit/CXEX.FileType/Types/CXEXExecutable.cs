using System;
using System.Collections.Generic;
using CXEX.Core.Interfaces;
using CXEX.FileType.Structures;
using CXEX.FileType.Parsers;

namespace CXEX.FileType.Types;

public class CXEXExecutable : ICXFile
{
    public CXEXHeader Header { get; private set; } = null!;
    public List<CXEXSection> Sections { get; private set; } = new();
    public CXEXSignatureBlock? Signature { get; private set; }

    // Holds the raw file backing so we can extract sections later
    private byte[] _rawData = Array.Empty<byte>();

    public string GetDisplayName()
    {
        return Header?.TypeCode switch
        {
            0x4B45 => "CXK Protected Kernel Executable (.xkex)",
            0x4245 => "CXK Boot Executive (.xbex)",
            0x4F45 => "CXOS System Executive (.xoex)",
            0x4345 => "CXOS User Application (.xcex)",
            _ => "Unknown CXEX Object"
        };
    }

    public void Load(byte[] data)
    {
        _rawData = data;
        ReadOnlySpan<byte> span = data;

        Header = CXEXParser.ParseHeader(span);

        int currentOffset = Header.SectionOffset;
        for (int i = 0; i < Header.SectionCount; i++)
        {
            Sections.Add(CXEXParser.ParseSection(span, currentOffset));
            currentOffset += CXEXParser.SECTION_SIZE;
        }

        if (Header.IsSigned)
        {
            Signature = CXEXParser.ParseSignature(span, Header.SignatureOffset);
        }
    }

    public byte[] GetSectionData(string sectionName)
    {
        var section = Sections.Find(s => s.Name == sectionName);
        if (section == null || section.FileSize == 0)
            return Array.Empty<byte>();

        byte[] dest = new byte[section.FileSize];
        Array.Copy(_rawData, section.FileOffset, dest, 0, section.FileSize);
        return dest;
    }
}