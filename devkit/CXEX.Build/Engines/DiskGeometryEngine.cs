using System;
using System.Collections.Generic;
using CXEX.Build.Layout;
using CXEX.Core.Constants;

namespace CXEX.Build.Engines;

public static class DiskGeometryEngine
{
    private const int SECTOR_SIZE = 512;
    private const int ALIGN_SECTORS = 2048; // 1 MiB alignment

    public static DiskGeometryMap CalculateLayout(int diskSizeMb, int bootSizeMb, int stage2Bytes, int stagedFilesBytes)
    {
        var map = new DiskGeometryMap
        {
            TotalSectors = (ulong)diskSizeMb * 1024 * 1024 / SECTOR_SIZE
        };

        ulong stage2Sectors = (ulong)(stage2Bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
        ulong cursorLba = AlignUp(map.Stage2Lba + stage2Sectors, ALIGN_SECTORS);

        // 1. BOOT Partition
        ulong bootSectors = (ulong)bootSizeMb * 1024 * 1024 / SECTOR_SIZE;
        map.Partitions.Add(new PartitionLayout
        {
            Label = "BOOT",
            TypeCode = CXFlags.PART_CXBOOT,
            Flags = CXFlags.PART_FLAG_BOOTABLE,
            StartLba = cursorLba,
            SectorCount = bootSectors
        });
        cursorLba += bootSectors;

        // 2. STAGE Partition (Only if we have files to stage)
        if (stagedFilesBytes > 0)
        {
            // Calculate sectors: 1 manifest sector + data sectors
            ulong stageDataSectors = (ulong)(stagedFilesBytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
            ulong stageTotalSectors = AlignUp(1 + stageDataSectors, ALIGN_SECTORS);

            map.Partitions.Add(new PartitionLayout
            {
                Label = "STAGE",
                TypeCode = CXFlags.PART_CXSTAGE,
                Flags = 0,
                StartLba = cursorLba,
                SectorCount = stageTotalSectors
            });
            cursorLba += stageTotalSectors;
        }

        // 3. SYSTEM Partition (Takes up the rest of the disk)
        if (cursorLba >= map.TotalSectors)
            throw new InvalidOperationException("Disk size is too small for the requested partition layout.");

        map.Partitions.Add(new PartitionLayout
        {
            Label = "SYSTEM",
            TypeCode = CXFlags.PART_CXFS,
            Flags = 0,
            StartLba = cursorLba,
            SectorCount = map.TotalSectors - cursorLba
        });

        return map;
    }

    private static ulong AlignUp(ulong value, ulong alignment) => (value + alignment - 1) / alignment * alignment;
}