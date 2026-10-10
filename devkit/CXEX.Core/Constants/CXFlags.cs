// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;

namespace CXEX.Core.Constants;

public static class CXFlags
{
    // Executable Type Codes.
    //
    // Each is the extension's domain letter followed by 'E' for executive, and
    // the domain says WHOSE the program is - which is what decides the
    // authority it starts with. 0x4345 'CE' is deliberately absent: .xcex named
    // an executable for how it was built, and every executable here is
    // compiled, so it distinguished nothing. See CX_EXTENSION_SYSTEM.md.
    public const ushort TYPE_KERNEL = 0x4B45; // 'KE' .xkex - ring 0
    public const ushort TYPE_BOOT = 0x4245;   // 'BE' .xbex - the boot chain
    public const ushort TYPE_OS = 0x4F45;     // 'OE' .xoex - the OS proper
    public const ushort TYPE_SYSTEM = 0x5345; // 'SE' .xsex - OS-owned, not the OS
    public const ushort TYPE_USER = 0x5545;   // 'UE' .xuex - a user's

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