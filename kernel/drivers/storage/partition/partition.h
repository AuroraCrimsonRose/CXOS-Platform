/* /kernel/drivers/storage/partition/partition.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Partition layer: a scheme-agnostic view of a disk's partitions. The kernel
 * asks "what partitions are on this disk" and "where is the CXFS one" without
 * caring how the table is stored; the on-disk format is a swappable backend.
 *
 * Current backend: XBPT (CX Boot Partition Table) - CATX's own table format,
 * for CXK-only disks (no GPT/MBR interop needed). Minimal by design; CRC and a
 * backup copy can be added later behind this same interface.
 *
 * ---- XBPT on-disk format (little-endian) ----------------------------------
 * Lives at a fixed location, XBPT_LBA (sector 1), so it never moves and the
 * bootloader/kernel find it without searching. One 512-byte sector:
 *
 *   header (32 bytes):
 *     0   4   magic        "XBPT"
 *     4   2   version      (1)
 *     6   2   entry_count  number of populated entries
 *     8   2   entry_size   bytes per entry (32) - for forward compatibility
 *     10  2   flags        (reserved, 0)
 *     12  8   disk_sectors total disk size in 512B sectors (for validation)
 *     20  12  reserved     (0)
 *
 *   then entry_count entries (32 bytes each, up to XBPT_MAX_ENTRIES):
 *     0   8   start_lba     first sector of the partition
 *     8   8   sector_count  length in sectors
 *     16  1   type          PART_TYPE_*
 *     17  1   flags         PART_FLAG_*
 *     18  2   reserved
 *     20  12  name          short label, NUL-padded (e.g. "BOOT","SYSTEM")
 *
 * 32 + 15*32 = 512, so one sector holds up to 15 partitions. LBA 0 stays the
 * boot sector; the table sits at LBA 1, raw stage2/kernel and the CXFS volume
 * live in the partitions the table describes.
 */

#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>

#define XBPT_LBA            1u          /* fixed sector holding the table */
#define XBPT_VERSION        1u
#define XBPT_HEADER_SIZE    32u
#define XBPT_ENTRY_SIZE     32u
#define XBPT_MAX_ENTRIES    15u         /* (512 - 32) / 32 */

/* partition type bytes (CXK-defined) */
#define PART_TYPE_EMPTY     0x00u
#define PART_TYPE_CXBOOT    0xCBu        /* raw boot area: stage2 + kernel.xkex */
#define PART_TYPE_CXFS      0xC5u        /* a CXFS volume */
#define PART_TYPE_CXSTAGE   0xCAu        /* first-boot staging payload (XSTG) */

/* partition flags */
#define PART_FLAG_BOOTABLE  0x01u

struct partition {
    uint64_t start_lba;     /* first sector */
    uint64_t sectors;       /* length in sectors */
    uint8_t  type;          /* PART_TYPE_* */
    uint8_t  flags;         /* PART_FLAG_* */
    char     name[13];      /* 12 chars + NUL */
};

/* Read disk `id`'s partition table into out[0..max-1]; *count gets the number
   found. Returns 0 on success, negative on read error / bad table. */
int part_scan(uint8_t id, struct partition *out, int max, int *count);

/* Find the first partition of `type` on disk `id`. Returns 0 and fills *out on
   success, negative if none / on error. */
int part_find_type(uint8_t id, uint8_t type, struct partition *out);

#endif