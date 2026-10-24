// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/pagedb.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The page allocator on x86-64: pmm.h over the page database and the buddy
 * allocator (memman/buddy.h), with a zone below 4 GB
 * (docs/kernel/CX_KERNEL_PLAN.md section 9.3). This header holds what it has
 * beyond pmm.h. The database is reached through the direct map, so it needs
 * one; x86-32 keeps its bitmap (memman/pmm.c) until it is deleted in roadmap
 * step 6.
 */

#ifndef MEMMAN_PAGEDB_H
#define MEMMAN_PAGEDB_H

#include <stddef.h>
#include <stdint.h>
#include "arch/paging.h"

/* One page for paging_init's tables, before pmm_init: usable RAM above the
   kernel image and below `below`, which the caller can reach through the
   loader's mapping. 0 when there is none left. pmm_init keeps every page
   handed out here; a call after pmm_init is a panic. Not zeroed. */
paddr_t pmm_boot_alloc(paddr_t below);

/* pmm_free_pages without the panic: 0, or -1 when a page in the range is not
   in use, and then nothing is freed. pmm_free and pmm_free_pages panic
   instead, since freeing a free page is a broken invariant; this lets the
   self-tests see the refusal without ending the boot. */
int pmm_free_checked(paddr_t first, size_t count);

/* Where the allocator keeps what it must never hand out, for the self-tests:
   the database itself, and the pages pmm_boot_alloc gave paging_init. */
void pagedb_database(paddr_t *start, uint64_t *bytes);
void pagedb_boot_tables(paddr_t *start, uint64_t *bytes);

#endif
