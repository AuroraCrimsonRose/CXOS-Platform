// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/paging.c */
/* Aurora Tejeda / CATX Systems */
/*
 * x86 32-bit paging - v5, RECURSIVE page directory.
 *
 * Unlike v4 (identity-mapped, enabled paging itself), v5 inherits paging ALREADY
 * ON from kernel.asm's higher-half bootstrap. paging_init does NOT re-enable
 * paging; it ADOPTS the existing boot page directory and installs the recursive
 * mapping so the kernel can edit page tables it can't otherwise reach.
 *
 * Recursive trick: PDE[1023] points at the page directory itself. Then:
 *   - the page directory is visible at virtual 0xFFFFF000
 *   - the page table for PD index i is visible at 0xFFC00000 + (i << 12)
 *   - the PTE mapping virtual V is at  0xFFC00000 + (PD_INDEX(V)<<12) + PT_INDEX(V)*4
 * This costs the top 4 MB of virtual space (0xFFC00000+), which the kernel
 * doesn't need. (Address math verified against known cases before writing this.)
 */

#include "paging.h"
#include "logging.h"
#include "color.h"
#include "pmm.h"

#define KERNEL_VBASE   0xC0000000u
#define RECURSIVE_SLOT 1023

/* A reserved higher-half scratch page for temporarily mapping an arbitrary
   physical frame so the kernel can read/write it (e.g. a not-yet-current page
   directory). Its PDE is reserved at init so temp-mapping only ever touches a
   PTE - never creates a new PDE - which keeps it reentrancy-safe. PDE index
   1022, inside the shared kernel half, just below the recursive window. */
#define TEMP_MAP_VADDR 0xFF800000u

/* Optional hook invoked when paging_map creates a NEW kernel-half PDE, so live
   address spaces can be kept in sync. Registered by the address-space layer. */
static void (*g_kernel_pde_hook)(uint32_t index, uint32_t value) = 0;
void paging_set_pde_hook(void (*fn)(uint32_t index, uint32_t value)) {
    g_kernel_pde_hook = fn;
}

/* the boot page directory built by kernel.asm (a kernel symbol => virtual addr).
   Its PHYSICAL address is (virtual - KERNEL_VBASE). */
extern uint32_t boot_page_dir[];

#define PD_INDEX(v)  (((v) >> 22) & 0x3FF)
#define PT_INDEX(v)  (((v) >> 12) & 0x3FF)

/* recursive virtual addresses */
#define PD_VIRT      ((volatile uint32_t *)0xFFFFF000u)
static inline volatile uint32_t *pt_virt(uint32_t v) {
    return (volatile uint32_t *)(0xFFC00000u + (PD_INDEX(v) << 12));
}

static inline uint32_t virt_to_phys_kernel(void *p) {
    return (uint32_t)p - KERNEL_VBASE;
}

static inline void invlpg(uint32_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

void paging_init(void) {
    /* install the recursive entry into the existing boot page directory:
       PDE[1023] -> the PD's own physical address, present + writable. */
    uint32_t pd_phys = virt_to_phys_kernel(boot_page_dir);
    boot_page_dir[RECURSIVE_SLOT] = pd_phys | PAGE_PRESENT | PAGE_WRITE;

    /* reload CR3 to flush the TLB so the recursive mapping takes effect. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");

    /* Reserve the temp-map region's PDE now, while we're in the kernel space and
       before any other address space exists, so the PDE is part of the shared
       kernel half (copied into every space) and temp-mapping later only writes a
       PTE - never creates a PDE (which keeps the PDE-creation hook from
       recursing). Map then unmap one page to force the PDE+PT into existence. */
    paging_map_kernel(TEMP_MAP_VADDR, 0, PAGE_PRESENT | PAGE_WRITE);
    paging_unmap(TEMP_MAP_VADDR);
}

/* The shared body. Private on purpose: it is the only code that can create a
   mapping with any combination of flags, so the two entry points below are the
   whole public surface. */
static void paging_map_raw(uint32_t virt, uint32_t phys, uint32_t flags);

/* Map a page for the KERNEL's own use: MMIO windows, the heap, kernel stacks,
   ACPI tables, the temp-map slot. It deliberately does NOT constrain the virtual
   address, because drivers legitimately map device memory wherever the PCI BAR
   put it - but it refuses PAGE_USER outright, so the only route to a ring-3
   mapping is paging_map_user below.
   That asymmetry is the point (security review §8): a kernel mapping may be
   anywhere but can never be reachable from ring 3, and a user mapping may carry
   the USER bit but can never name a kernel address. Neither call can express the
   dangerous combination, so no caller has to remember not to. */
void paging_map_kernel(uint32_t virt, uint32_t phys, uint32_t flags) {
    /* Not a silent strip. A caller passing PAGE_USER here means to create a
       ring-3 mapping and has reached for the wrong function; carrying on with
       the bit removed would give it a mapping that silently does not work. */
    if (flags & PAGE_USER) {
        klog("PAGING", SEV_FAIL, "paging_map_kernel called with PAGE_USER - use paging_map_user");
        return;
    }
    paging_map_raw(virt, phys, flags);
}

/* Map a page into the ring-3 half. Returns 0, or -1 if the address is not in
   the user half - which is the whole reason this exists. The CXEX loader reaches
   the page tables through here, and an image naming 0xC0000000 would otherwise
   have had the kernel map its own memory with the USER bit set (security
   review §1). */
int paging_map_user(uint32_t virt, uint32_t phys, uint32_t flags) {
    if (virt >= KERNEL_VBASE) return -1;
    paging_map_raw(virt, phys, flags | PAGE_USER);
    return 0;
}

static void paging_map_raw(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pdi = PD_INDEX(virt);

    /* ensure a page table exists for this region. PD is at PD_VIRT via recursion. */
    if (!(PD_VIRT[pdi] & PAGE_PRESENT)) {
        /* allocate a fresh page table frame (physical) from the PMM */
        uint32_t new_pt = (uint32_t)pmm_alloc();
        if (!new_pt) return;   /* out of memory */
        /* install it into the PD: present + writable (+ user if requested) */
        PD_VIRT[pdi] = (new_pt & ~0xFFFu) | PAGE_PRESENT | PAGE_WRITE
                     | (flags & PAGE_USER);
        /* the new table is now reachable at pt_virt(virt); flush + zero it. */
        invlpg((uint32_t)pt_virt(virt));
        volatile uint32_t *t = pt_virt(virt);
        for (int i = 0; i < PAGE_ENTRIES; i++) t[i] = 0;

        /* If we just created a PDE in the shared kernel half, let live address
           spaces pick it up. (Excludes the recursive slot 1023; the temp-map
           PDE is reserved before any space exists, so it never lands here at
           runtime - which also means this never recurses through temp-map.) */
        if (g_kernel_pde_hook && pdi >= 768 && pdi <= 1022)
            g_kernel_pde_hook(pdi, PD_VIRT[pdi]);
    }

    /* write the PTE through the recursive page-table window */
    volatile uint32_t *table = pt_virt(virt);
    table[PT_INDEX(virt)] = (phys & ~0xFFFu) | (flags & 0xFFF) | PAGE_PRESENT;

    /* CPU ANDs privilege bits across BOTH levels: make sure the PDE also grants
       USER/WRITE if this mapping needs them (the PDE may have been created
       supervisor-only by an earlier mapping). */
    if (flags & PAGE_USER)  PD_VIRT[pdi] |= PAGE_USER;
    if (flags & PAGE_WRITE) PD_VIRT[pdi] |= PAGE_WRITE;

    invlpg(virt);
}

void paging_unmap(uint32_t virt) {
    uint32_t pdi = PD_INDEX(virt);
    if (!(PD_VIRT[pdi] & PAGE_PRESENT)) return;   /* no table => nothing mapped */
    volatile uint32_t *table = pt_virt(virt);
    table[PT_INDEX(virt)] = 0;
    invlpg(virt);
}

/* True iff `virt` is mapped present AND ring-3 accessible in the ACTIVE address
   space. Lets user_ptr_readable and user_ptr_writable validate ring-3 pointers per-process: a process's CR3
   is live during its own syscalls, so this reads that process's tables. */
/* Both levels must carry every required bit, because the CPU ANDs the privilege
   bits across the PDE and the PTE: a writable PTE under a read-only PDE is not
   writable, and checking only one level would say it was. */
static int paging_has(uint32_t virt, uint32_t need) {
    uint32_t pdi = PD_INDEX(virt);
    if ((PD_VIRT[pdi] & need) != need) return 0;
    volatile uint32_t *table = pt_virt(virt);
    return (table[PT_INDEX(virt)] & need) == need;
}

int paging_is_user(uint32_t virt) {
    return paging_has(virt, PAGE_PRESENT | PAGE_USER);
}

/* Present, ring-3 accessible AND writable.
   paging_is_user alone answers "may ring 3 touch this", which is not the
   question a syscall writing into a user buffer is asking: a read-only user page
   passes it. When this split was introduced CR0.WP was clear, so the kernel's
   write then went through regardless and this check was the only thing refusing
   it (security review §4). WP is set now (kernel.asm), so the CPU refuses that
   write as well - but this remains the policy answer rather than a duplicate of
   it, because WP enforces only the WRITE bit: it says nothing about PAGE_USER,
   which this also requires. The caller must still ask for the access it
   intends. */
int paging_is_user_writable(uint32_t virt) {
    return paging_has(virt, PAGE_PRESENT | PAGE_USER | PAGE_WRITE);
}

uint32_t paging_get_phys(uint32_t virt) {
    uint32_t pdi = PD_INDEX(virt);
    if (!(PD_VIRT[pdi] & PAGE_PRESENT)) return 0;
    volatile uint32_t *table = pt_virt(virt);
    uint32_t entry = table[PT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    return (entry & ~0xFFFu) | (virt & 0xFFFu);
}
/* ---- temporary single-page mapping of an arbitrary physical frame ----------
   Map `phys` into the reserved scratch slot and return a pointer to it; the
   caller reads/writes, then calls paging_temp_unmap. Lets the kernel touch a
   frame that isn't otherwise mapped (e.g. a freshly allocated page directory of
   a not-yet-active address space). Not reentrant: one frame at a time. */
void *paging_temp_map(uint32_t phys) {
    paging_map_kernel(TEMP_MAP_VADDR, phys & ~0xFFFu, PAGE_PRESENT | PAGE_WRITE);
    return (void *)TEMP_MAP_VADDR;
}

void paging_temp_unmap(void) {
    paging_unmap(TEMP_MAP_VADDR);
}