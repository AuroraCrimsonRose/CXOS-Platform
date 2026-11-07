// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/cxfs_one.h */
/* Aurora Tejeda / CATX Systems */
/*
 * One hostile disk through CXFS: the body of the CXFS fuzz target
 * (fuzz_cxfs.c) and of the regression tests made from what it found
 * (test_cxfs.c), so a crash input replays in the host suite exactly as the
 * fuzzer ran it.
 *
 * The input is a disk an attacker attached (CX_FILESYSTEM.md section 9):
 *
 *   bytes 0-3    the device's size, 512-byte sectors (little-endian)
 *   bytes 4-7    the LBA the volume is probed at (a partition's start)
 *   bytes 8-9    fail every disk transfer after this many (0xFFFF: never)
 *   bytes 10-11  reserved, 0
 *   bytes 12-    the device's contents from that LBA on; the rest reads zero
 *
 * It is mounted twice: as the root volume (cxfs_mount_at, which has the
 * manifest cache), then as a second volume beside a good root
 * (cxfs_mount_volume, the path cxk_mount_extra_volumes takes at boot). On
 * each, every read-only operation runs, then a create, write, read-back and
 * delete, then the same on the files the disk itself lists.
 *
 * The oracle, besides the sanitizers:
 *   - every transfer stays inside the device and inside the volume, whole
 *     4 KB blocks, block 0 read only by the mount;
 *   - nothing past block 0 is touched, and nothing mounted, unless
 *     cxfs_sb_validate accepts the superblock;
 *   - each operation touches only the regions it has reason to: entry reads
 *     the manifest, file reads the manifest and the data region, writers the
 *     bitmap, manifest and data region; never the superblock;
 *   - the good root volume beside a hostile one is held to the same rules;
 *   - what was written reads back the same, when no transfer was failed.
 */
#ifndef CXFS_ONE_H
#define CXFS_ONE_H

#include <stdint.h>
#include <stddef.h>

#define CXFS_ONE_HEADER 12

/* How far the last input got: what keeps the target from passing because it
   stopped reaching the code (test_cxfs.c holds the real seeds to it). */
struct cxfs_one_stats {
    int mounted_root;      /* step 1 mounted it */
    int mounted_second;    /* step 2 mounted it */
    int listed;            /* root-directory entries seen, both steps */
    int files_read;        /* of those, files read to the end without error */
    int written_back;      /* our own file written and read back the same */
};
extern struct cxfs_one_stats cxfs_one_last;

/* Runs one input. NULL when every rule held, else the first one broken. */
const char *cxfs_one(const uint8_t *data, size_t size);

#endif
