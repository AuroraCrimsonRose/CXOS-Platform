/* /kernel/memman/vmregion.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Per-process memory mappings - the kernel half of SYS_MEM_OP.
 *
 * A ring-3 process starts with an image and a stack and nothing else. This is
 * where it gets more: a table of the regions each process has mapped, the
 * placement policy that decides where a new one lands, and the quota that
 * bounds how much one process may hold at once.
 *
 * WHY A TABLE AT ALL, given that the page tables already record every mapping.
 * Three things the page tables cannot answer: which pages were handed out as
 * ONE request (so UNMAP can refuse to release half of something), how much a
 * process has taken (so the quota means anything), and what a mapping is a
 * VIEW of - its object and 64-bit offset - which has no room in a PTE. When
 * file-backed mappings arrive, that last one is what makes them possible.
 *
 * WHAT THIS DOES NOT DO: free frames on exit. addr_space_reclaim_user() walks
 * the whole user half and frees every present frame, so an exiting process's
 * mappings are already reclaimed by the address-space teardown that was there
 * before this existed. This module only forgets its bookkeeping. Duplicating
 * the free here would be a double-free, which is worth stating plainly because
 * the symmetry of "map allocates, so unmap-on-exit should free" is exactly the
 * wrong instinct in this one place.
 */

#ifndef VMREGION_H
#define VMREGION_H

#include <stdint.h>
#include "cxk_abi.h"

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

/* The quota a process gets when its parent asks for none, and the most any
   process may be given. Starts at VM_DEFAULT_QUOTA. */
uint32_t vm_default_quota(void);
void     vm_set_default_quota(uint32_t bytes);

/* Begin tracking `pid`, with a ceiling of min(quota, the parent's ceiling) so
   authority over memory attenuates down a spawn chain the way grants do.
   `parent` < 0 means there is none (the kernel-launched executive).

   Honest limit: this bounds how much any ONE process may hold, not how much a
   process and its descendants may hold together. A process that spawns without
   end still exhausts RAM, more slowly. Fixing that needs a budget charged to a
   whole subtree, which is a different and larger design. */
void vm_proc_init(int pid, uint32_t quota, int parent);

/* Forget pid's regions. Call on exit AFTER addr_space_reclaim_user(), which is
   what actually frees the frames. */
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
   is rounded up to a page. Returns E_OK, or E_NOMEM if it would pass the
   ceiling (E_RANGE if the rounding or the sum would wrap). vm_uncharge gives it
   back, for a caller that unwinds a placement it has already paid for; a
   process that dies instead needs nothing, since vm_proc_reset drops the whole
   account. */
int  vm_charge(int pid, uint32_t bytes);
void vm_uncharge(int pid, uint32_t bytes);

/* The SYS_MEM_OP operations. `a` has already been validated as a readable and
   writable user pointer by the caller. Each returns E_OK or a negative E_*. */
int vm_map(int pid, struct mem_op_args *a);
int vm_unmap(int pid, uint32_t addr, uint32_t length);
int vm_info(int pid, struct mem_op_args *a);

#endif
