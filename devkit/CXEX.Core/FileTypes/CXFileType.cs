using System.Runtime.InteropServices;

namespace CXEX.Core.FileTypes;

// Used by all .xkex, .xoex, .xbex
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct CXEXHeader
{
    public unsafe fixed byte Magic[4];
    public ushort TypeCode;
    public ushort FormatVersion;
    public ushort ArchTarget;
    public ushort AbiVersion;
    public uint Flags;
    public uint EntryPoint;
    public uint LoadBase;
    public uint ImageMin;
    public uint ImageMax;
    public ushort SectionCount;
    public ushort SectionOffset;
    public uint RelocOffset;
    public uint SignatureOffset;
    public uint DependencyOffset;
    public unsafe fixed byte Reserved[8];
}

// Used by all .xkpk, .xksk
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct CXPKHeader
{
    public unsafe fixed byte Magic[4];
    public ushort Version;
    public ushort KeyBits;
    public uint Exponent;
    public ushort ModulusLen;
    public ushort Reserved;
}