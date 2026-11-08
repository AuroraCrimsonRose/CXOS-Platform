// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kmem.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Memory objects (KOBJ_MEM; docs/kernel/CX_KERNEL_PLAN.md section 9.4).
 *
 * A memory object holds pages; a mapping (memman/vmregion.h) is a view of
 * one. Its pages arrive on the first touch: the page fault asks kmem_page
 * for the page at an index, and a page nobody has touched is taken then and
 * zeroed (Serenity's AnonymousVMObject, Haiku's VMCache).
 *
 * STRICT COMMIT. Every page an object could ever hold is promised when it is
 * made: its size is charged to a job (cpu/job.h) and that many physical pages
 * are reserved (pmm_reserve), so a first touch always finds one. Running out
 * is the creator's E_NO_MEMORY, never a fault with nothing behind it. When
 * the object goes, its pages are freed and what was reserved and not taken is
 * given back, and so is the job's charge.
 *
 * The object owns its pages. A mapping holds a reference on the object and
 * maps its pages VM_BORROWED, so an address space torn down leaves them; the
 * object's own last reference frees them.
 *
 * The page array is under the object's lock, so a fault and the object's
 * other users (two mappings, other processes) see each page appear
 * once.
 *
 * SEALS (1.3e). An object travels in IPC messages, so a receiver may share it
 * with a process it does not trust. A seal binds every holder: MEM_SEAL_WRITE
 * means no writable mapping exists or can be made, and since a mapping is
 * the only way into the pages (a kernel copy goes through the caller's
 * mapping and its protection), the bytes are fixed. The writable mappings are
 * counted under the lock, so a seal and a new mapping cannot both win (Linux
 * memfd's F_SEAL_WRITE, which is refused while a shared writable mapping
 * exists).
 *
 * KINDS (2.2). Most objects are KMEM_ANON, as above. A driver's are not
 * (drivers/devhost.h): KMEM_DEVICE is a device's registers, every page
 * present from the start, none of them the allocator's, mapped uncached and
 * never freed; KMEM_DMA is one run of pages below 4 GB, all present and
 * charged at once, whose physical address the device is given. Both hold a
 * reference on their device. A DMA object's pages outlive its last
 * reference while the device is open (kmem_dma_release): the device may
 * still be writing them.
 *
 * KMEM_SCREEN (2.3) is the framebuffer's pages, given to the display server
 * (drivers/video/fb.h, SYS_SCREEN_OPEN): present from the start, the
 * allocator's never, mapped write-combining, charged to nobody, and at most
 * one at a time. Its last reference gives the screen back to the kernel.
 */
#ifndef CXK_KMEM_H
#define CXK_KMEM_H

#include <stdint.h>
#include "object/kobj.h"
#include "sync/spinlock.h"
#include "arch/paging.h"

struct job;
struct kdevice;   /* drivers/devhost.h */

enum kmem_kind {
    KMEM_ANON = 0,   /* pages on first touch, committed, the object's own */
    KMEM_DEVICE,     /* a device's registers: fixed, uncached, nobody's to free */
    KMEM_DMA,        /* contiguous below 4 GB, present now, freed after the device */
    KMEM_SCREEN,     /* the framebuffer: fixed, write-combining, one at a time (2.3) */
};

struct kmem {
    struct kobj  hdr;        /* first; KOBJ_MEM */
    spinlock_t   lock;       /* pages and resident, writers and seals */
    uint32_t     npages;     /* its size; never 0 */
    uint32_t     resident;   /* pages taken so far */
    uint32_t     writers;    /* writable mappings of it now */
    uint32_t     seals;      /* MEM_SEAL_*; only ever added */
    uint32_t     lent;       /* 1 while a lease lives: no writable mapping (2.4) */
    paddr_t     *pages;      /* npages entries, 0 until touched (kmalloc) */
    struct job  *job;        /* charged npages pages; a reference (0: KMEM_DEVICE) */
    uint32_t     kind;       /* enum kmem_kind */
    paddr_t      phys;       /* KMEM_DEVICE, KMEM_DMA, KMEM_SCREEN: the first page's address */
    struct kdevice *dev;     /* KMEM_DEVICE, KMEM_DMA: a reference */
    struct kmem *qnext;      /* KMEM_DMA: in its device's list of retired buffers */
};

void kmem_sys_init(void);

/* A new object of `npages` (1 to MMAP_MAX_BYTES / PAGE_SIZE) zero pages,
   committed now and charged to `job`, with one reference for the caller.
   0 with *rc set: E_NO_MEMORY past the job's limit, past what memory can
   promise, or when the kernel has no room for its records. */
struct kmem *kmem_create(uint32_t npages, struct job *job, int *rc);

/* A device's registers: `npages` pages from `base`, with a reference on
   `dev`, charged to nobody. 0 with *rc set (E_NO_MEMORY: no room for the
   records; E_OUT_OF_RANGE: npages past MMAP_MAX_BYTES / PAGE_SIZE). */
struct kmem *kmem_create_device(paddr_t base, uint32_t npages, struct kdevice *dev, int *rc);

/* The screen's `npages` pages from `base`, charged to nobody, holding no
   device. fb.c makes it once it has the screen; its destroy calls
   fb_screen_release. 0 with *rc set as kmem_create_device. */
struct kmem *kmem_create_screen(paddr_t base, uint32_t npages, int *rc);

/* A DMA buffer of `npages` (1 to DMA_ALLOC_MAX / PAGE_SIZE) zeroed pages in
   one run below 4 GB, charged to `job`, with a reference on `dev`; the
   first page's address in m->phys. 0 with *rc set: E_NO_MEMORY past the
   job's limit, or with no such run free past the kernel's margin. */
struct kmem *kmem_create_dma(uint32_t npages, struct job *job, struct kdevice *dev, int *rc);

/* A retired DMA buffer's pages, charge and records, once its device can no
   longer reach them (drivers/devhost.c, as the device closes). */
void kmem_dma_release(struct kmem *m);

/* The page-table flags its kind adds to a mapping: VM_UNCACHED for a
   device's registers, VM_WC for the screen, 0 otherwise. */
uint32_t kmem_map_flags(const struct kmem *m);

/* The page at `index` (< npages), taken and zeroed if this is its first
   touch. Never fails: the page was reserved when the object was made. */
paddr_t kmem_page(struct kmem *m, uint32_t index);

/* The page at `index` if it has been touched, or 0. */
paddr_t kmem_page_if_resident(struct kmem *m, uint32_t index);

/* Pages it holds now. */
uint32_t kmem_resident(struct kmem *m);

/* A writable mapping of it is being made: E_ACCESS_DENIED if it is sealed
   against writing, E_BUSY while it is lent, else E_OK and counted until
   kmem_writer_drop. */
int  kmem_writer_add(struct kmem *m);
void kmem_writer_drop(struct kmem *m);

/* Add `seals` (MEM_SEAL_*, checked by the caller). E_ACCESS_DENIED once it
   has MEM_SEAL_SEAL, E_BUSY for MEM_SEAL_WRITE while a writer is counted;
   nothing is added unless all of it is. */
int kmem_seal(struct kmem *m, uint32_t seals);

/* Its seals now. */
uint32_t kmem_seals(struct kmem *m);

/* A lease on it (KOBJ_LEASE, one reference for the caller): until the lease's
   last reference goes, no writable mapping of it can be made. 0 with *rc set:
   E_BUSY while a writer is counted or a lease lives, E_NOT_SUPPORTED but for
   KMEM_ANON, E_NO_MEMORY. The check and the taking are one step under the
   object's lock, as a seal's are. */
struct kobj *kmem_lend(struct kmem *m, int *rc);

/* Is `lease` (a KOBJ_LEASE) on `m`? */
int kmem_lease_is_on(struct kobj *lease, struct kmem *m);

#endif
