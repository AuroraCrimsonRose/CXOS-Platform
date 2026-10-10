// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using CXEX.Build.Layout;
using CXEX.Core.Utilities;

namespace CXEX.Build.Emitters;

public class StagedFile
{
    public string Name { get; set; } = string.Empty;
    public byte[] Data { get; set; } = Array.Empty<byte>();
}

public static class XBPTImageWriter
{
    public static void WriteImage(string outPath, DiskGeometryMap map, byte[] stage1, byte[] stage2, byte[] kernel, List<StagedFile> stagedFiles)
    {
        long diskSizeBytes = (long)map.TotalSectors * map.SectorSize;
        byte[] diskImage = new byte[diskSizeBytes];

        void Put(ulong lba, byte[] data) => Array.Copy(data, 0, diskImage, (long)lba * map.SectorSize, data.Length);

        // 1. Stage 1 (LBA 0)
        byte[] s1 = new byte[512];
        if (stage1 != null && stage1.Length > 0) Array.Copy(stage1, s1, Math.Min(stage1.Length, 510));
        WriteProtectiveMbr(s1, map);
        s1[510] = 0x55; s1[511] = 0xAA;
        Put(map.Stage1Lba, s1);

        // 2. Stage 2 (LBA 2)
        if (stage2 != null) Put(map.Stage2Lba, stage2);

        // 3. Write Partitions
        foreach (var part in map.Partitions)
        {
            if (part.Label == "BOOT" && kernel != null)
            {
                Put(part.StartLba, kernel);

                // CRITICAL FIX: Patch the KSNT marker in Stage 2 with the kernel sector count
                PatchStage2KernelSectors(diskImage, map.Stage2Lba, kernel.Length);
            }
            else if (part.Label == "STAGE" && stagedFiles.Count > 0)
            {
                WriteStagedPayload(diskImage, part.StartLba, stagedFiles);
            }
        }

        // 4. XBPT Table (LBA 1)
        Put(map.XbptTableLba, SerializeXbpt(map));

        File.WriteAllBytes(outPath, diskImage);
    }

    /// <summary>
    /// Writes a <b>protective</b> MBR partition table into sector 0, at the classic
    /// offset 446. This does not replace or compete with XBPT, which remains the
    /// real partition table and stays exactly where it is, at LBA 1 - a different
    /// sector entirely. The two never touch.
    ///
    /// <para>It exists only to satisfy firmware. A BIOS booting USB media decides
    /// between USB-HDD and USB-FDD emulation partly by looking for a partition table
    /// here; finding 64 zero bytes, many pick floppy emulation, and then reject the
    /// disk for not having a valid FAT BPB. The symptom is a black screen with a
    /// blinking cursor and no output, because stage 1 never runs at all. This is the
    /// same trick GPT plays with its own protective MBR, and for the same reason:
    /// occupy the legacy structure so legacy firmware sees a disk that is spoken for
    /// rather than one that appears blank.</para>
    ///
    /// <para>One entry, marked active, covering the disk from LBA 1 onward. Type
    /// 0xDA is "non-FS data" - it is honest about the disk not holding a filesystem
    /// any OS knows, and it stops Windows and Linux offering to mount or format it.
    /// If some particular firmware turns out to want a different type byte, this is
    /// the one value to change.</para>
    /// </summary>
    private const byte ProtectiveMbrType = 0xDA;   // non-FS data

    private static void WriteProtectiveMbr(byte[] sector0, DiskGeometryMap map)
    {
        const int PartitionTableOffset = 446;

        // Never clobber boot code. Stage 1 is padded to leave 446..509 free, and
        // if some future stage 1 grows past that, corrupting it silently would be
        // far worse than shipping without the protective entry.
        for (int i = PartitionTableOffset; i < 510; i++)
        {
            if (sector0[i] != 0x00) return;
        }

        ulong start = 1;                                   // LBA 0 is this sector
        ulong count = map.TotalSectors > start ? map.TotalSectors - start : 0;
        if (count == 0) return;
        if (count > uint.MaxValue) count = uint.MaxValue;  // MBR fields are 32-bit

        int p = PartitionTableOffset;
        sector0[p + 0] = 0x80;                 // active / bootable
        sector0[p + 1] = 0x00;                 // start CHS: head 0
        sector0[p + 2] = 0x02;                 //            sector 2, cylinder 0
        sector0[p + 3] = 0x00;
        sector0[p + 4] = ProtectiveMbrType;
        sector0[p + 5] = 0xFF;                 // end CHS: beyond CHS range, as
        sector0[p + 6] = 0xFF;                 // GPT's protective MBR also does
        sector0[p + 7] = 0xFF;
        MemoryPrimitives.WriteU32(sector0.AsSpan(), p + 8, (uint)start);
        MemoryPrimitives.WriteU32(sector0.AsSpan(), p + 12, (uint)count);

        // Entries 1-3 stay zero: one entry is all firmware needs to see.
    }

    /// <summary>
    /// How many sectors stage 2 actually reads: <c>KERNEL_SECTORS</c> in
    /// boot/stage2.asm. It is a hard cap, not a hint — the read loop reads exactly
    /// this many, and a larger kernel simply has its tail left on disk.
    ///
    /// <para>Keep the two in step. The constant was 512 for a long time with a
    /// comment reading "Kernel is ~50KB now: 5x room"; the kernel reached 99.4% of
    /// it, and the next few KB anyone added silently truncated <c>.text</c>. The
    /// only symptom was one self-test failing — the X Data object is linked last,
    /// so it is the first thing to fall off the end — which points nowhere near
    /// the boot loader.</para>
    /// </summary>
    private const int Stage2KernelSectorBudget = 1024;   // 512 KB

    private static void PatchStage2KernelSectors(byte[] diskImage, ulong stage2Lba, int kernelByteSize)
    {
        int kernelSectors = (int)Math.Ceiling((double)kernelByteSize / 512);

        // Refuse rather than ship an image whose kernel is cut off at boot. The
        // failure this replaces had no error of its own: the machine booted, ran,
        // and one unrelated-looking self-test failed.
        if (kernelSectors > Stage2KernelSectorBudget)
            throw new InvalidDataException(
                $"the kernel is {kernelByteSize} bytes ({kernelSectors} sectors), beyond the " +
                $"{Stage2KernelSectorBudget} sectors stage 2 reads ({Stage2KernelSectorBudget * 512} bytes). " +
                "Raise KERNEL_SECTORS in boot/stage2.asm and Stage2KernelSectorBudget here together, " +
                "keeping the load buffer at 0x10000 clear of the protected-mode stack at 0x9F000.");

        int searchStart = (int)(stage2Lba * 512);
        int searchEnd = searchStart + (16 * 512);

        for (int i = searchStart; i < searchEnd - 4; i++)
        {
            if (diskImage[i] == 0x54 && diskImage[i + 1] == 0x4E && diskImage[i + 2] == 0x53 && diskImage[i + 3] == 0x4B)
            {
                CXEX.Core.Utilities.MemoryPrimitives.WriteU32(diskImage.AsSpan(), i + 4, (uint)kernelSectors);
                return;
            }
        }

        // MAKE SURE THIS IS A RETURN, NOT A THROW
        return;
    }

    /* The XSTG manifest: a 16-byte header, then 48 bytes per file, running on
       over as many sectors as they need - up to 16 - with the count of sectors at
       offset 8.

       The version was a number in this comment and nowhere else, so "the kernel's
       install.h says 2" was a claim no build could check. It is a constant now,
       declared in versions.json as `xstg` and checked against XSTG_VERSION in
       kernel/drivers/storage/install.h. */
    public const int XstgVersion = 2;
    public const int XstgEntrySize = 48;
    public const int XstgNameLen = 32;
    public const int XstgMaxSectors = 16;

    public static int ManifestSectors(int files) => (16 + files * XstgEntrySize + 511) / 512;

    private static void WriteStagedPayload(byte[] diskImage, ulong startLba, List<StagedFile> files)
    {
        int msecs = ManifestSectors(files.Count);
        if (msecs > XstgMaxSectors)
            throw new InvalidOperationException($"{files.Count} staged files: a manifest holds at most {(XstgMaxSectors * 512 - 16) / XstgEntrySize}");

        Span<byte> manifest = diskImage.AsSpan((int)(startLba * 512), msecs * 512);
        MemoryPrimitives.WriteU32(manifest, 0, 0x47545358); // "XSTG"
        MemoryPrimitives.WriteU16(manifest, 4, 2);
        MemoryPrimitives.WriteU16(manifest, 6, (ushort)files.Count);
        MemoryPrimitives.WriteU16(manifest, 8, (ushort)msecs);

        int manifestOffset = 16;
        uint currentSectorOffset = (uint)msecs; // data blobs follow the manifest

        foreach (var file in files)
        {
            // A name is a path, NUL-padded to 32 bytes - cut short, it would
            // install the file somewhere else without a word.
            byte[] nameBytes = Encoding.ASCII.GetBytes(file.Name);
            if (nameBytes.Length > XstgNameLen)
                throw new InvalidOperationException($"staged name '{file.Name}' is longer than {XstgNameLen} bytes");
            nameBytes.CopyTo(manifest.Slice(manifestOffset, nameBytes.Length));

            MemoryPrimitives.WriteU32(manifest, manifestOffset + 32, currentSectorOffset);
            MemoryPrimitives.WriteU32(manifest, manifestOffset + 44, (uint)file.Data.Length);

            // Write the actual file data into the disk image
            Array.Copy(file.Data, 0, diskImage, (long)(startLba + currentSectorOffset) * 512, file.Data.Length);

            currentSectorOffset += (uint)((file.Data.Length + 511) / 512);
            manifestOffset += XstgEntrySize;
        }
    }

    private static byte[] SerializeXbpt(DiskGeometryMap map)
    {
        byte[] sector = new byte[512];
        Span<byte> span = sector;

        MemoryPrimitives.WriteU32(span, 0, 0x54504258); // "XBPT"
        MemoryPrimitives.WriteU16(span, 4, 1);
        MemoryPrimitives.WriteU16(span, 6, (ushort)map.Partitions.Count);
        MemoryPrimitives.WriteU16(span, 8, 32); // 32 bytes per entry
        MemoryPrimitives.WriteU64(span, 12, map.TotalSectors);

        int offset = 32;
        foreach (var part in map.Partitions)
        {
            MemoryPrimitives.WriteU64(span, offset, part.StartLba);
            MemoryPrimitives.WriteU64(span, offset + 8, part.SectorCount);
            span[offset + 16] = part.TypeCode;
            span[offset + 17] = part.Flags;

            byte[] label = Encoding.ASCII.GetBytes(part.Label);
            label.CopyTo(span.Slice(offset + 20, Math.Min(label.Length, 12)));

            offset += 32;
        }

        return sector;
    }
}