/* /kernel/memman/addr_space.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Per-process address spaces. Each space is its own page directory that SHARES
 * the kernel's higher half (so kernel code/stack/data stay mapped no matter
 * which space is active) and has its OWN recursive slot and its OWN user half.
 *
 * Layout of every space's page directory (1024 PDEs):
 *   [0    .. 767 ]  user half        - private to the space (empty at create)
 *   [768  .. 1022]  kernel half      - SHARED: copied from the kernel PD, so all
 *                                      spaces see the same kernel (0xC0000000+).
 *   [1023]          recursive slot   - points at THIS PD (per space).
 *
 * Because the kernel half is shared, switching CR3 between spaces never unmaps
 * the running kernel. To load an image into a new space you create it, switch
 * to it, then map with the normal pager (which now edits that space).
 *
 * Sharing note: the kernel half is copied at create time. Changes WITHIN an
 * existing kernel page table (e.g. growing the heap inside an already-present
 * PDE) are seen by all spaces automatically, because they share the same
 * physical page tables. Adding a brand-new kernel PDE (a new 4 MB higher-half
 * region) after spaces exist would NOT propagate - establish kernel higher-half
 * regions before creating spaces, or sync that PDE into live spaces.
 */

#ifndef ADDR_SPACE_H
#define ADDR_SPACE_H

#include <stdint.h>

struct addr_space {
    uint32_t pd_phys;     /* physical address of this space's page directory */
};

/* PDE index range of the shared kernel higher half (0xC0000000 >> 22 = 768),
   up to but not including the recursive slot (1023, which is per-space). */
#define ADDR_SPACE_KERNEL_PDE_LO  768u
#define ADDR_SPACE_KERNEL_PDE_HI  1022u
#define ADDR_SPACE_RECURSIVE_PDE  1023u

/* Initialise a fresh page directory for a new space (PURE - no hardware, so it
   is unit-testable). `new_pd` must be 1024 accessible entries; `kernel_pd` is
   the kernel's directory to copy the shared half from; `new_pd_phys` is the
   physical address of new_pd (for the recursive slot). */
void addr_space_init_pd(uint32_t *new_pd, const uint32_t *kernel_pd,
                        uint32_t new_pd_phys);

/* Register the kernel space and arm the kernel-half PDE-creation hook. Call
   once after paging_init, before creating any other space. */
void addr_space_init(void);

/* Propagate a newly created shared-kernel-half PDE into every live space. */
void addr_space_propagate_pde(uint32_t index, uint32_t value);

/* Create a new address space (allocate + init a PD). 0 on success, -1 on OOM. */
int  addr_space_create(struct addr_space *out);

/* Fill *out with the kernel's own space, so a caller can switch back to it. */
void addr_space_kernel(struct addr_space *out);

/* Make `s` the active address space (load CR3). */
void addr_space_switch(const struct addr_space *s);

/* Unregister a space (does not yet free its page tables/PD). */
void addr_space_destroy(const struct addr_space *s);

/* free the user half (frames + page tables) of the ACTIVE space; call before
   switching away + addr_space_destroy() to fully reclaim an exited process. */
void addr_space_reclaim_user(void);

#endif