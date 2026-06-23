using System;

namespace CXEX.Core.Constants;

public static class CXFlags
{
    // Executable Type Codes
    public const ushort TYPE_KERNEL = 0x4B45; // 'KE'
    public const ushort TYPE_BOOT = 0x4245; // 'BE'
    public const ushort TYPE_USER = 0x4345; // 'CE'
    public const ushort TYPE_OS = 0x4F45; // 'OE'

    // Header Flags (CX_EXTENSION_SYSTEM.md 9.6)
    public const uint FLAG_EXECUTABLE = 1 << 0;
    public const uint FLAG_RELOCATABLE = 1 << 1;
    public const uint FLAG_SIGNED = 1 << 2;
    public const uint FLAG_KERNEL_PRIV = 1 << 3;
    public const uint FLAG_REQUIRE_ABI_MATCH = 1 << 4;
    public const uint FLAG_REQUIRE_ARCH_MATCH = 1 << 5;

    // Section Flags (9.4)
    public const uint SEC_READ = 1 << 0;
    public const uint SEC_WRITE = 1 << 1;
    public const uint SEC_EXEC = 1 << 2;
    public const uint SEC_NOBITS = 1 << 3;

    // XBPT Partition Types
    public const byte PART_CXBOOT = 0xCB;
    public const byte PART_CXSTAGE = 0xCA;
    public const byte PART_CXFS = 0xC5;

    // Partition Flags
    public const byte PART_FLAG_BOOTABLE = 0x01;
}