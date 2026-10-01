/* /CXK/kernel/drivers/storage/install.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * First-boot install (hybrid model). A CXK disk ships with a SYSTEM partition
 * that is zeroed (no filesystem yet) and a STAGE partition (type 0xCA) holding
 * the /System payload behind a small manifest. On first boot the kernel formats
 * the SYSTEM partition as CXFS, creates /System, and copies the staged files in
 * using its own filesystem driver - so there is ONE CXFS implementation and the
 * disk becomes self-populating. On later boots the SYSTEM partition mounts and
 * install is skipped.
 *
 * ---- staging payload format (XSTG, little-endian) in the STAGE partition ----
 *   sectors 0 .. manifest_sectors-1 (header + manifest):
 *     0   4   magic            "XSTG"
 *     4   2   version          (2)
 *     6   2   file_count
 *     8   2   manifest_sectors 1 .. XSTG_MAX_SECTORS
 *     10  6   reserved
 *     16  ..  entries (file_count), 48 bytes each, running on across the
 *             manifest's sectors:
 *         0   32  name           target PATH relative to the root, NUL-padded
 *                                (e.g. "Shared/Programs/hi.xuex"). Missing
 *                                parent directories are created. A name with
 *                                no '/' lands in the root.
 *         32  4   start_sector   blob location, sectors from the partition start
 *         44  4   size_bytes     blob length in bytes
 *   sector start_sector.. : each file's raw bytes (sector-aligned).
 *
 * Version 1 was a single sector - at most 10 files - and the disk outgrew it.
 */

#ifndef INSTALL_H
#define INSTALL_H

#include <stdint.h>

#define XSTG_VERSION      2u
#define XSTG_MAX_SECTORS  16u     /* (16*512 - 16) / 48 = 170 files */
#define XSTG_NAME_LEN     32u
#define XSTG_ENTRY_SIZE   48u

enum cxk_install_result {
    CXK_INSTALL_MOUNTED    = 0,   /* SYSTEM already had a filesystem; mounted it */
    CXK_INSTALL_DONE       = 1,   /* formatted + populated /System this boot */
    CXK_INSTALL_NO_SYSTEM  = -1,  /* no CXFS partition found */
    CXK_INSTALL_FORMAT_ERR = -2,  /* format/mount of SYSTEM failed */
    CXK_INSTALL_STAGE_ERR  = -3   /* staging payload missing/!corrupt (formatted but empty) */
};

/* Ensure the SYSTEM partition is a mounted CXFS volume: mount it if it already
   has one, otherwise format it and populate /System from the STAGE partition.
   Leaves CXFS mounted on success. Returns a cxk_install_result. */
int cxk_install_first_boot(uint8_t disk_id);

/* Find the disk that carries an XBPT table, point CXFS at it, and run the
   first-boot install on it. Returns a cxk_install_result. */
int cxk_install_boot_disk(void);

/* Put every drive that is not the boot drive under /Drives, and mount each
   CXFS volume it holds inside its drive's directory, named by the volume's own
   label: /Drives/HDD1/Data. Returns how many volumes were mounted. Safe to
   call with no extra drives; a drive whose filesystem this kernel does not
   recognise gets its directory and no volumes, which is not an error. */
int cxk_mount_extra_volumes(uint8_t boot_disk_id);

#endif