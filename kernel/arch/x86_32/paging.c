// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/paging.c */
/* Aurora Tejeda / CATX Systems */
/*
 * arch/paging.h on 32-bit x86: a RECURSIVE page directory.
 *
 * Paging is already on when this runs: entry.asm's higher-half bootstrap
 * enabled it. paging_init ADOPTS the boot page directory and installs the
 * recursive mapping so the kernel can edit page tables it can't otherwise reach.
 *
 * Recursive trick: PDE[1023] points at the page directory itself. Then:
 *   - the page directory is visible at virtual 0xFFFFF000
 *   - the page table for PD index i is visible at 0xFFC00000 + (i << 12)
 *   - the PTE mapping virtual V is at  0xFFC00000 + (PD_INDEX(V)<<12) + PT_INDEX(V)*4
 * This costs the top 4 MB of virtual space (0xFFC00000+), which the kernel
 * doesn't need.
 */

#include "arch/x86_32/pagetable.h"
#include "logging.h"
#include "pmm.h"

/* A reserved higher-half scratch page for temporarily mapping an arbitrary
   physical frame so the kernel can read/write it (e.g. a not-yet-current page
   directory). Its PDE is reserved at init so temp-mapping only ever touches a
   PTE - never creates a new PDE - which keeps it reentrancy-safe. PDE index
   1022, inside the shared kernel half, just below the recursive window. */
#define TEMP_MAP_VADDR 0xFF800000u

/* Set once the address-space layer is up (addr_space_init): from then on a new
   kernel-half PDE is pushed into every live space. */
static int g_propagate = 0;
void paging_propagate_on(void) { g_propagate = 1; }

/* the boot page directory built by entry.asm (a kernel symbol => virtual addr).
   Its PHYSICAL address is (virtual - KERNEL_VBASE). */
extern uint32_t boot_page_dir[];

#define PD_INDEX(v)  (((v) >> 22) & 0x3FF)
#define PT_INDEX(v)  (((v) >> 12) & 0x3FF)

/* recursive virtual addresses */
#define PD_VIRT      ((volatile uint32_t *)0xFFFFF000u)
static inline volatile uint32_t *pt_virt(uint32_t v) {
    return (volatile uint32_t *)(0xFFC00000u + (PD_INDEX(v) << 12));
}

static inline void invlpg(uint32_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

static void map_raw(uint32_t virt, paddr_t phys, uint32_t vm, int user);

/* The boot directory is adopted, and the tables it grows come from the bitmap. */
void arch_memory_init(void) {
    pmm_init();
    paging_init();
}

void paging_init(void) {
    /* install the recursive entry into the existing boot page directory:
       PDE[1023] -> the PD's own physical address, present + writable. */
    uint32_t pd_phys = (uint32_t)boot_page_dir - KERNEL_VBASE;
    boot_page_dir[PD_RECURSIVE] = pd_phys | PTE_PRESENT | PTE_WRITE;

    /* reload CR3 to flush the TLB so the recursive mapping takes effect. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");

    /* Reserve the temp-map region's PDE now, while we're in the kernel space and
       before any other address space exists, so the PDE is part of the shared
       kernel half (copied into every space) and temp-mapping later only writes a
       PTE - never creates a PDE (which keeps propagation from recursing through
       the temp map). Map then unmap one page to force the PDE+PT into existence. */
    map_raw(TEMP_MAP_VADDR, 0, VM_WRITE, 0);
    paging_unmap(TEMP_MAP_VADDR);
}

/* Map a page for the KERNEL's own use: MMIO windows, the heap, kernel stacks,
   ACPI tables, the temp-map slot. It deliberately does NOT constrain the virtual
   address, because drivers legitimately map device memory wherever the PCI BAR
   put it - but nothing a caller passes can make it reachable from ring 3, so the
   only route to a ring-3 mapping is paging_map_user below.
   That asymmetry is the point (security review §8): a kernel mapping may be
   anywhere but can never be reachable from ring 3, and a user mapping can never
   name a kernel address. Neither call can express the dangerous combination, so
   no caller has to remember not to. */
void paging_map_kernel(uintptr_t virt, paddr_t phys, uint32_t vm) {
    /* Not a silent strip: a flag this interface doesn't define is a caller
       passing something else (a raw PTE bit, say) by mistake. */
    if (vm & ~VM_ALL) {
        klog("PAGING", SEV_WARN, "paging_map_kernel refused: not a VM_* flag");
        return;
    }
    map_raw(virt, phys, vm, 0);
}

/* Where it sits: virtual = physical. Device registers must lie in the kernel
   half: only it is shared by every address space, so registers mapped below
   it would fault the moment a driver touched them from a syscall. Firmware
   tables are read at boot, before any process exists. */
volatile void *paging_map_phys(paddr_t phys, uint32_t len, uint32_t vm) {
    paddr_t last = phys + len - 1;   /* inclusive: a range may end at 4 GB */
    if (len == 0 || last < phys) return 0;
    if ((vm & VM_UNCACHED) && phys < KERNEL_VBASE) {
        klog_u32("PAGING", SEV_WARN, "device registers below the kernel half: ", phys,
                 LOG_COLOR_VALUE, " - not visible from a process address space");
        return 0;
    }
    for (paddr_t a = phys & ~0xFFFu; ; a += PAGE_SIZE) {
        paging_map_kernel(a, a, vm);
        if (a == (last & ~0xFFFu)) break;
    }
    return (volatile void *)phys;
}

/* Map a page into the ring-3 half. Returns 0, or -1 if the address is not in
   the user half - which is the whole reason this exists. The CXEX loader reaches
   the page tables through here, and an image naming 0xC0000000 would otherwise
   have had the kernel map its own memory with the USER bit set (security
   review §1). */
int paging_map_user(uintptr_t virt, paddr_t phys, uint32_t vm) {
    if (virt >= KERNEL_VBASE) return -1;
    if (vm & ~VM_ALL) return -1;
    map_raw(virt, phys, vm, 1);
    return 0;
}

static void map_raw(uint32_t virt, paddr_t phys, uint32_t vm, int user) {
    uint32_t pdi = PD_INDEX(virt);

    /* ensure a page table exists for this region. PD is at PD_VIRT via recursion. */
    if (!(PD_VIRT[pdi] & PTE_PRESENT)) {
        /* allocate a fresh page table frame (physical) from the PMM */
        uint32_t new_pt = (uint32_t)pmm_alloc();
        if (!new_pt) return;   /* out of memory */
        /* install it into the PD: present + writable (+ user if requested) */
        PD_VIRT[pdi] = (new_pt & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE
                     | (user ? PTE_USER : 0);
        /* the new table is now reachable at pt_virt(virt); flush + zero it. */
        invlpg((uint32_t)pt_virt(virt));
        volatile uint32_t *t = pt_virt(virt);
        for (uint32_t i = 0; i < PT_ENTRIES; i++) t[i] = 0;

        /* If we just created a PDE in the shared kernel half, let live address
           spaces pick it up. (Excludes the recursive slot 1023; the temp-map
           PDE is reserved before any space exists, so it never lands here at
           runtime - which also means this never recurses through temp-map.) */
        if (g_propagate && pdi >= PD_KERNEL_LO && pdi <= PD_KERNEL_HI)
            addr_space_propagate_pde(pdi, PD_VIRT[pdi]);
    }

    /* write the PTE through the recursive page-table window */
    volatile uint32_t *table = pt_virt(virt);
    table[PT_INDEX(virt)] = pte_encode(phys, vm, user);

    /* CPU ANDs privilege bits across BOTH levels: make sure the PDE also grants
       USER/WRITE if this mapping needs them (the PDE may have been created
       supervisor-only by an earlier mapping). */
    if (user)           PD_VIRT[pdi] |= PTE_USER;
    if (vm & VM_WRITE)  PD_VIRT[pdi] |= PTE_WRITE;

    invlpg(virt);
}

void paging_unmap(uintptr_t virt) {
    uint32_t pdi = PD_INDEX(virt);
    if (!(PD_VIRT[pdi] & PTE_PRESENT)) return;   /* no table => nothing mapped */
    volatile uint32_t *table = pt_virt(virt);
    table[PT_INDEX(virt)] = 0;
    invlpg(virt);
}

/* True iff `virt` is mapped present AND ring-3 accessible in the ACTIVE address
   space. Lets user_ptr_readable and user_ptr_writable validate ring-3 pointers
   per-process: a process's CR3 is live during its own syscalls, so this reads
   that process's tables. Both levels must carry every required bit, because the
   CPU ANDs the privilege bits across the PDE and the PTE: a writable PTE under a
   read-only PDE is not writable, and checking only one level would say it was. */
static int paging_has(uint32_t virt, uint32_t need) {
    uint32_t pdi = PD_INDEX(virt);
    if ((PD_VIRT[pdi] & need) != need) return 0;
    volatile uint32_t *table = pt_virt(virt);
    return (table[PT_INDEX(virt)] & need) == need;
}

int paging_is_user(uintptr_t virt) {
    return paging_has(virt, PTE_PRESENT | PTE_USER);
}

/* Present, ring-3 accessible AND writable.
   paging_is_user alone answers "may ring 3 touch this", which is not the
   question a syscall writing into a user buffer is asking: a read-only user page
   passes it. When this split was introduced CR0.WP was clear, so the kernel's
   write then went through regardless and this check was the only thing refusing
   it (security review §4). WP is set now (entry.asm), so the CPU refuses that
   write as well - but this remains the policy answer rather than a duplicate of
   it, because WP enforces only the WRITE bit: it says nothing about PTE_USER,
   which this also requires. The caller must still ask for the access it
   intends. */
int paging_is_user_writable(uintptr_t virt) {
    return paging_has(virt, PTE_PRESENT | PTE_USER | PTE_WRITE);
}

paddr_t paging_get_phys(uintptr_t virt) {
    uint32_t pdi = PD_INDEX(virt);
    if (!(PD_VIRT[pdi] & PTE_PRESENT)) return 0;
    volatile uint32_t *table = pt_virt(virt);
    uint32_t entry = table[PT_INDEX(virt)];
    if (!(entry & PTE_PRESENT)) return 0;
    return (entry & PTE_ADDR_MASK) | (virt & 0xFFFu);
}

/* ---- temporary single-page mapping of an arbitrary physical frame ----------
   Map `phys` into the reserved scratch slot and return a pointer to it; the
   caller reads/writes, then calls paging_temp_unmap. Lets the kernel touch a
   frame that isn't otherwise mapped (e.g. a freshly allocated page directory of
   a not-yet-active address space). Not reentrant: one frame at a time. */
void *paging_temp_map(paddr_t phys) {
    map_raw(TEMP_MAP_VADDR, phys & PTE_ADDR_MASK, VM_WRITE, 0);
    return (void *)TEMP_MAP_VADDR;
}

void paging_temp_unmap(void) {
    paging_unmap(TEMP_MAP_VADDR);
}

int paging_kernel_obeys_readonly(void) {
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & (1u << 16)) != 0;   /* CR0.WP */
}
