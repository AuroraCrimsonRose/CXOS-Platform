/* /CXLite/kernel/memman/heap.h */
/* Aurora Tejeda */
/*
 * Kernel heap: arbitrary-size dynamic allocation on top of the PMM.
 *
 * kmalloc(size) / kfree(ptr) are the ONLY allocation front door. Keep all
 * dynamic allocation going through them so the implementation can change
 * underneath without touching callers.
 *
 * v1 is a linked-list (free-list) allocator for all sizes. See heap.c for
 * the documented seam where slab caches for small fixed sizes would later
 * be inserted (the Linux model: kmalloc routes small sizes to slab caches,
 * larger ones to the general allocator).
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