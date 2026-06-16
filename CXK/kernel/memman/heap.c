/* /CXLite/kernel/memman/heap.c */
/* Aurora Tejeda */
/* Kernel heap - v1 linked-list (free-list) allocator on top of the PMM. */

#include "heap.h"
#include "paging.h"
#include "pmm.h"

/* Each block (free or allocated) is preceded by this header. The heap is a
   doubly-ish linked list walked in address order via `next`. */
typedef struct block {
    size_t        size;     /* usable bytes in this block (excludes header) */
    int           free;     /* 1 = free, 0 = allocated */
    struct block *next;     /* next block by address (NULL = last) */
} block_t;

#define BLOCK_HEADER_SIZE  (sizeof(block_t))
#define MIN_SPLIT          (BLOCK_HEADER_SIZE + 16)  /* don't split tiny remainders */
#define ALIGN8(x)          (((x) + 7) & ~((size_t)7))

static block_t *head = 0;          /* first block in the heap */
static uint32_t total_bytes = 0;   /* total usable bytes across all blocks */
static uint32_t used_bytes = 0;

/* grow the heap by requesting pages from the PMM, appending one big free
   block. Returns the new block, or 0 if the PMM is out of memory. */
static block_t *heap_grow(size_t need) {
    /* round up to whole pages */
    size_t bytes = ALIGN8(need) + BLOCK_HEADER_SIZE;
    size_t pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    void *mem = pmm_alloc_pages(pages);
    if (!mem) return 0;

    /* The PMM can hand out physical pages above the kernel's initial identity
       map (paging_init only maps low memory + the framebuffer). Those pages are
       unmapped, so writing the block header below would page-fault. Map each
       page we just got (identity: virt = phys) before touching it. */
    uint32_t base = (uint32_t)mem;
    for (size_t i = 0; i < pages; i++)
        paging_map(base + i * PMM_PAGE_SIZE,
                   base + i * PMM_PAGE_SIZE,
                   PAGE_PRESENT | PAGE_WRITE);

    block_t *b = (block_t *)mem;
    b->size = pages * PMM_PAGE_SIZE - BLOCK_HEADER_SIZE;
    b->free = 1;
    b->next = 0;

    total_bytes += b->size;

    /* append to the end of the list (keep address order) */
    if (!head) {
        head = b;
    } else {
        block_t *cur = head;
        while (cur->next) cur = cur->next;
        cur->next = b;
    }
    return b;
}

void heap_init(void) {
    head = 0;
    total_bytes = 0;
    used_bytes = 0;
    heap_grow(PMM_PAGE_SIZE);   /* start with one page of arena */
}

/* split `b` so it holds exactly `size` usable bytes; leftover becomes a new
   free block after it (only if the leftover is worth keeping). */
static void split_block(block_t *b, size_t size) {
    if (b->size >= size + MIN_SPLIT) {
        block_t *rest = (block_t *)((uint8_t *)b + BLOCK_HEADER_SIZE + size);
        rest->size = b->size - size - BLOCK_HEADER_SIZE;
        rest->free = 1;
        rest->next = b->next;

        b->size = size;
        b->next = rest;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return 0;
    size = ALIGN8(size);

    /* ---- SIZE-ROUTING SEAM ----------------------------------------------
       v1 sends every request through the general free-list below.
       FUTURE (slab): route small fixed sizes (e.g. <= 256 bytes, rounded to
       size classes) to per-size slab caches here for speed and to cut
       fragmentation; fall through to the free list for large/odd sizes.
       Callers never change - they only ever call kmalloc().
       --------------------------------------------------------------------- */

    /* first-fit: walk the list for a free block big enough */
    block_t *b = head;
    while (b) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            used_bytes += b->size;
            return (uint8_t *)b + BLOCK_HEADER_SIZE;
        }
        b = b->next;
    }

    /* nothing fit - grow the heap and use the new block */
    b = heap_grow(size);
    if (!b) return 0;
    split_block(b, size);
    b->free = 0;
    used_bytes += b->size;
    return (uint8_t *)b + BLOCK_HEADER_SIZE;
}

/* merge `b` with its immediate next neighbor if both are free and adjacent */
static void coalesce(block_t *b) {
    while (b->next && b->free && b->next->free &&
           (uint8_t *)b + BLOCK_HEADER_SIZE + b->size == (uint8_t *)b->next) {
        b->size += BLOCK_HEADER_SIZE + b->next->size;
        b->next = b->next->next;
    }
}

void kfree(void *ptr) {
    if (!ptr) return;

    block_t *b = (block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
    if (b->free) return;          /* double-free guard */
    b->free = 1;
    used_bytes -= b->size;

    /* coalesce forward, and also try from head to merge a preceding block */
    coalesce(b);
    block_t *cur = head;
    while (cur && cur->next) {
        if (cur->free) coalesce(cur);
        cur = cur->next;
    }
}

uint32_t heap_bytes_used(void) { return used_bytes; }
uint32_t heap_bytes_free(void) { return total_bytes - used_bytes; }