namespace CXEX.FileSystem.Volume;

public static class CXFSConstants
{
    public const uint Magic = 0x43584653; // "CXFS"
    public const int BlockSize = 4096;
    public const int NameLen = 64;
    public const int MaxExtents = 8;
    public const int EntrySize = 256;

    // Entry Types
    public const byte TypeFree = 0;
    public const byte TypeFile = 1;
    public const byte TypeDir = 2;

    // Feature Flags
    public const uint FeatTimestamps = 0x01;
    public const uint FeatPerms = 0x02;
    public const uint FeatLocking = 0x04;
    public const uint FeatLargeBlk = 0x08;
}