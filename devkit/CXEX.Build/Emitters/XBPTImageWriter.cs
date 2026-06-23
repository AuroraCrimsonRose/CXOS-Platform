using System;
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

    private static void PatchStage2KernelSectors(byte[] diskImage, ulong stage2Lba, int kernelByteSize)
    {
        int kernelSectors = (int)Math.Ceiling((double)kernelByteSize / 512);
        int searchStart = (int)(stage2Lba * 512);
        int searchEnd = searchStart + (16 * 512); // Max 16 sectors for stage 2

        for (int i = searchStart; i < searchEnd - 4; i++)
        {
            // Search for "KSNT" (0x54, 0x4E, 0x53, 0x4B)
            if (diskImage[i] == 0x54 && diskImage[i + 1] == 0x4E && diskImage[i + 2] == 0x53 && diskImage[i + 3] == 0x4B)
            {
                // Write 4-byte sector count immediately after the marker
                MemoryPrimitives.WriteU32(diskImage.AsSpan(), i + 4, (uint)kernelSectors);
                return;
            }
        }
        throw new InvalidDataException("KSNT marker not found in Stage 2. Kernel size cannot be patched.");
    }

    private static void WriteStagedPayload(byte[] diskImage, ulong startLba, List<StagedFile> files)
    {
        Span<byte> manifestSector = diskImage.AsSpan((int)(startLba * 512), 512);
        MemoryPrimitives.WriteU32(manifestSector, 0, 0x47545358); // "XSTG"
        MemoryPrimitives.WriteU16(manifestSector, 4, 1);
        MemoryPrimitives.WriteU16(manifestSector, 6, (ushort)files.Count);

        int manifestOffset = 16;
        uint currentSectorOffset = 1; // Data blobs start at sector offset 1

        foreach (var file in files)
        {
            // CRITICAL FIX: Exactly 48 bytes per entry as required by install.h
            byte[] nameBytes = Encoding.ASCII.GetBytes(file.Name);
            nameBytes.CopyTo(manifestSector.Slice(manifestOffset, Math.Min(nameBytes.Length, 32)));

            MemoryPrimitives.WriteU32(manifestSector, manifestOffset + 32, currentSectorOffset);
            MemoryPrimitives.WriteU32(manifestSector, manifestOffset + 44, (uint)file.Data.Length);

            // Write the actual file data into the disk image
            Array.Copy(file.Data, 0, diskImage, (long)(startLba + currentSectorOffset) * 512, file.Data.Length);

            currentSectorOffset += (uint)((file.Data.Length + 511) / 512);
            manifestOffset += 48;
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