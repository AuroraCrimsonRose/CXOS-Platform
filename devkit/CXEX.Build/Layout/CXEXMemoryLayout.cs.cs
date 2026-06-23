using System;
using System.Collections.Generic;

namespace CXEX.Build.Layout;

public class CxexMemoryLayout
{
    public ushort TypeCode { get; set; }
    public ushort FormatVersion { get; set; } = 1;
    public ushort ArchTarget { get; set; } = 1;
    public ushort AbiVersion { get; set; } = 1;
    public uint Flags { get; set; }

    public uint EntryPoint { get; set; }
    public uint LoadBase { get; set; }
    public uint PhysBase { get; set; }
    public uint ImageMin { get; set; }
    public uint ImageMax { get; set; }

    public List<SectionLayout> Sections { get; set; } = new();
}

public class SectionLayout
{
    public string Name { get; set; } = string.Empty;
    public uint VirtualAddress { get; set; }
    public uint FileOffset { get; set; }
    public uint FileSize { get; set; }
    public uint MemSize { get; set; }
    public uint Flags { get; set; }
    public byte[] Payload { get; set; } = Array.Empty<byte>();
}