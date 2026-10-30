// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/vmregion.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Per-process memory mappings - the kernel half of the SYS_VM_* calls.
 *
 * A ring-3 process starts with an image and a stack and nothing else. This is
 * where it gets more: a record of the regions each process has mapped, the
 * placement policy that decides where a new one lands, and the quota that
 * bounds how much one process may hold at once.
 *
 * WHY A RECORD AT ALL, given that the page tables already record every
 * mapping. Three things the page tables cannot answer: which pages were
 * handed out as ONE request (so UNMAP can refuse to release half of
 * something), how much a process has taken (so the quota means anything), and
 * what a mapping is a VIEW of - its memory object and 64-bit offset - which
 * has no room in a PTE. Since 1.3d that last one is what fills a page: pages
 * arrive on the first touch (vm_fault), from the object (object/kmem.h).
 *
 * The regions are in a balanced tree (memman/vmtree.h), under the process's
 * lock, so a fault finds its region in O(log n).
 *
 * WHO FREES WHAT. A memory object owns its pages; each region holds a
 * reference on its object and maps the pages VM_BORROWED. Unmapping a region,
 * or resetting the account, unmaps its pages from the process's space and
 * drops the reference, and the object frees its pages when its last
 * reference goes. addr_space_release() frees the image, the stack and the
 * page tables, and passes over a borrowed page.
 *
 * WHAT IS CHARGED WHERE. A memory object charges its whole size to its job
 * when it is made (strict commit, object/kmem.h). The quota here counts what
 * the process maps, and what vm_charge adds for the image and stack, which
 * vm_charge charges to the job as well. So an anonymous mapping costs its
 * length once in each: the job through its object, the quota through the
 * region.
 */

#ifndef VMREGION_H
#define VMREGION_H

#include <stdint.h>
#include "cxk_abi.h"
#include "memman/vmtree.h"
#include "sync/spinlock.h"

/* Where mappings are placed. The user half holds the image at 0x00400000 (1 MiB
   cap) and the stack plus argument page just under 0xC0000000, so this window
   sits between them with a wide margin on both sides - far enough above the
   image that it can grow, far enough below the stack that a runaway stack hits
   an unmapped guard rather than a mapping. */
#define VM_MMAP_BASE 0x10000000u   /* 256 MiB */
#define VM_MMAP_TOP  0xB0000000u   /* ~2.75 GiB; 256 MiB clear of the stack */

/* The ceiling a process is given when nothing narrows it. A bound on one
   process, NOT a budget shared with its children - see vm_proc_init. */
#define VM_DEFAULT_QUOTA (16u * 1024u * 1024u)   /* built in; kernel.xkco may change it */

struct kmem;

struct vm_region {
    struct vm_node node;   /* first: base and len, page-aligned */
    uint32_t       prot;   /* MPROT_* as requested */
    struct kmem   *mem;    /* what this is a view of; a reference */
    uint64_t       off;    /* offset into it, page-aligned */
    /* A thread's stack (vm_map_stack): its lowest `guard` bytes are no
       page of the object, so running off the bottom faults, and only the
       kernel unmaps it. 0 and 0 for every other region. */
    uint32_t       guard;
    uint32_t       stack;
};

/* One process's mappings and quota. It lives in the process (cpu/proc.h);
   the functions below find it by pid. */
struct vm_proc {
    int             live;
    spinlock_t      lock;      /* root and count, against a fault */
    uint32_t        quota;     /* ceiling in bytes */
    uint32_t        mapped;    /* how much of it is in use: regions and charges */
    uint32_t        charged;   /* the part vm_charge took, charged to the job too */
    uint32_t        count;     /* regions */
    struct vm_node *root;      /* the regions, by address */
};

/* Set up the region cache. Once, at boot. */
void vm_sys_init(void);

/* The quota a process gets when its parent asks for none, and the most any
   process may be given. Starts at VM_DEFAULT_QUOTA. */
uint32_t vm_default_quota(void);
void     vm_set_default_quota(uint32_t bytes);

/* Begin tracking `pid`, with a ceiling of min(quota, the parent's ceiling) so
   authority over memory attenuates down a spawn chain the way grants do.
   `parent` < 0 means there is none (the kernel-launched executive). An
   account already live is reset first. */
void vm_proc_init(int pid, uint32_t quota, int parent);

/* Unmap pid's regions from its space and drop their objects, and give the
   job back what vm_charge took. Call before addr_space_release(): the space
   must still be there. A process with no space of its own (the self-tests')
   mapped into the kernel's, and is unmapped from it. */
void vm_proc_reset(int pid);

/* pid's ceiling in bytes, or 0 if it is not tracked. */
uint32_t vm_quota_of(int pid);

/* Charge user pages that are NOT one of pid's mmap regions: the pages the CXEX
   loader places an image into, and the stack and argument pages spawn builds
   around it. They are as real as a mapping - the same frames, held for the same
   lifetime - so leaving them out meant `mapped` under-reported what a process
   actually held, and an image of any size the loader's own page cap allowed
   could be placed with the quota already full (security review §3).

   Charged BEFORE the frames are allocated, so a refusal costs nothing. `bytes`
   is rounded up to a page. Returns E_OK, or E_NO_MEMORY if it would pass the
   ceiling (E_OUT_OF_RANGE if the rounding or the sum would wrap). It is
   charged to the job as well. vm_uncharge gives it back, for a caller that
   unwinds a placement it has already paid for; a process that dies instead
   needs nothing, since vm_proc_reset drops the whole account. */
int  vm_charge(int pid, uint32_t bytes);
void vm_uncharge(int pid, uint32_t bytes);

/* SYS_VM_MAP, SYS_VM_UNMAP and SYS_VM_INFO. `a` and `u` have already been
   validated as readable and writable user pointers by the caller. Each returns
   E_OK or a negative E_*.

   vm_map_object maps `m` (a->object named it; the caller holds a reference
   and keeps it) with the handle's `rights`, or, when `m` is 0, a new
   anonymous object made for the mapping alone, which a->object must be
   MOBJ_ANON for. vm_map is vm_map_object with no object. Nothing is mapped
   in the page tables yet: each page arrives on its first touch. */
int vm_map_object(int pid, struct vm_map_args *a, struct kmem *m, uint32_t rights);
int vm_map(int pid, struct vm_map_args *a);
int vm_unmap(int pid, uint64_t addr, uint64_t length);
int vm_info(int pid, struct vm_usage *u);

/* A user thread's stack (SYS_THREAD_CREATE; Haiku create_thread_user_stack):
   `len` bytes, page-aligned, of a new anonymous object, placed like a
   mapping with one unmapped guard page below it in the same region, so no
   other mapping can land there. Charged as a mapping of `len` (the guard
   costs address space, not memory). *base is the region's base, the guard's;
   *top is the stack's top. SYS_VM_UNMAP refuses it (E_ACCESS_DENIED): the
   thread on it would fault where it stands. E_OK, E_NO_MEMORY,
   E_NO_RESOURCES. */
int vm_map_stack(int pid, uint32_t len, uint64_t *base, uint64_t *top);

/* The stack vm_map_stack placed at `base` goes: unmapped, its object
   dropped, its charge given back. E_NOT_FOUND if there is none there, which
   is not an error once the process's whole account has been reset. */
int vm_unmap_stack(int pid, uint64_t base);

/* Where a futex word lives (sync/futex.c; Serenity get_futex_key): 1, with
   *mem the memory object of the region holding `addr` and *off the word's
   byte offset in it, if one of pid's regions holds it; the caller then owns
   a reference to *mem. 0, and nothing taken, if none does (the image, the
   first thread's stack, a stack's guard), and then the word is pid's own. */
int vm_futex_key(int pid, uint64_t addr, struct kmem **mem, uint64_t *off);

/* A first touch: the page holding `addr` is in one of pid's regions, the
   access (`write`, `exec`) is one the region allows, and the page is now
   mapped in pid's space. 1 if so; 0, with nothing mapped, if not, and then
   the fault is the process's. The page fault calls it for a page that was
   not present; so do the user-pointer checks (cpu/usercopy.c), so a buffer
   the kernel writes into need not have been touched first; and so do the
   self-tests, for a process that is not the current one. */
int vm_fault(int pid, uint64_t addr, int write, int exec);

#endif
