// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/hostdisk.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The storage seam for kernel code built for the host (CX_SAFETY.md 3.7).
 *
 * Kernel code above the disk registry - CXFS today - calls disk_read,
 * disk_write, disk_flush, disk_find_by_id and the storage lock exactly as it
 * does in the kernel. On the host those land here: RAM disks, sparse (a
 * device of any size costs only the 4 KB pages that hold something), with the
 * registry's own bounds rule (disk.c's disk_check) in front of every
 * transfer, so the code under test sees the errors the kernel would give it.
 *
 * A watcher sees every transfer the code under test asks for, refused or not.
 * That is what the CXFS fuzz target's oracle is built on: a driver that
 * addresses a block it had no business touching is a finding even when the
 * access stays inside the device and no sanitizer would notice.
 */
#ifndef HOSTDISK_H
#define HOSTDISK_H

#include <stdint.h>
#include <stddef.h>

#define HD_MAX_DISKS 4

/* Drop every disk and its contents, and the watcher. */
void hd_reset(void);

/* A blank (all-zero) disk of `sectors` 512-byte sectors under registry id
   `id`. Returns 0, or -1 when every slot is taken or the id is in use. */
int  hd_attach(uint8_t id, uint64_t sectors);

/* Write `len` bytes at byte offset `off` straight into the disk, past the
   watcher and the bounds rule: how a test puts an image on it. Bytes past the
   device's end are dropped. */
void hd_poke(uint8_t id, uint64_t off, const void *src, size_t len);
/* Read the same way. Unwritten bytes read as zero. */
void hd_peek(uint8_t id, uint64_t off, void *dst, size_t len);

/* Every disk_read / disk_write the code under test makes, after the bounds
   rule decided it: `rc` is what the caller is about to be given. */
typedef void (*hd_watch_fn)(uint8_t id, uint64_t lba, uint32_t count,
                            int write, int rc);
void hd_watch(hd_watch_fn fn);

/* Make the next `n` transfers succeed and every one after fail with
   DISK_ERR_FAULT, until hd_reset. n < 0 turns it off (the default). */
void hd_fail_after(int n);

#endif
