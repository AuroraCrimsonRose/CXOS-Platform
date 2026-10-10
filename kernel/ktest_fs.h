// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_fs.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Adversarial cases for CXFS (the 2026-10-09 platform security review §3,
 * Phase 2.5 in docs/planning/HARDENING_PLAN.md).
 *
 * Kept out of ktest.c for the reason ktest_loader.c and ktest_net.c are: these
 * bring their own structure builder. The superblock half needs no disk at all -
 * cxfs_sb_validate is a pure function over a struct, so every case is a
 * superblock built in memory and handed straight to it, and the filesystem's
 * mount-time validation is covered on every machine rather than only one with a
 * second drive attached.
 *
 * ktest.c already covers CXFS functionally (mount, create/write/read, the
 * offset layer, volume tags). None of that was adversarial: the inputs were
 * all well-formed. These are the malformed ones.
 */

#ifndef KTEST_FS_H
#define KTEST_FS_H

/* Every malformed superblock cxfs_sb_validate must refuse, plus the well-formed
   ones it must accept - including the live volume's own superblock when one is
   mounted. Needs no disk. 1 = all cases behaved, 0 = at least one did not. */
int ktest_cxfs_sb_adversarial(void);

/* Manifest entries: extents that address blocks outside the volume, the
   corrupt extent_len that used to zero the manifest through the bitmap flush,
   and names that arrive off the disk without a terminator. Skips (passes) when
   no CXFS is mounted. */
int ktest_cxfs_entry_adversarial(void);

#endif
