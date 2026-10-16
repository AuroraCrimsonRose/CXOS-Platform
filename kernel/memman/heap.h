// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/memman/heap.h */
/* Aurora Tejeda */
/*
 * Kernel heap: arbitrary-size dynamic allocation on top of the PMM.
 *
 * kmalloc(size) / kfree(ptr) are the ONLY allocation front door. Keep all
 * dynamic allocation going through them so the implementation can change
 * underneath without touching callers.
 *
 * Sizes up to 2048 bytes come from power-of-two size-class caches (ocache.h),
 * zeroed; larger ones from a first-fit linked list, which stops below the
 * caches' window. kfree tells them apart by address. (The Linux model:
 * kmalloc routes small sizes to slab caches, larger ones to the general
 * allocator.) heap_bytes_used / heap_bytes_free describe the list only.
 *
 * Kernel objects do not come from here at all: each type has its own cache.
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

/* initialize the heap (must run after pmm_init) */
void heap_init(void);

/* allocate `size` bytes, returns pointer or 0 on failure */
void *kmalloc(size_t size);

/* free a pointer previously returned by kmalloc (NULL is a safe no-op) */
void kfree(void *ptr);

/* stats */
uint32_t heap_bytes_used(void);
uint32_t heap_bytes_free(void);

#endif