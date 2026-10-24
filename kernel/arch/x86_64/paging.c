// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/paging.c */
/* Aurora Tejeda / CATX Systems */
/*
 * arch/paging.h on x86-64: four-level tables, reached through the direct map
 * (docs/kernel/CX_KERNEL_PLAN.md sections 2.4 and 9.5).
 *
 * The loader's tables map the first GiB twice with 2 MiB pages, writable and
 * executable (boot/long.asm). paging_init replaces them rather than adopting
 * them: it builds the kernel's own PML4 with every kernel-half entry made, the
 * direct map of RAM (no-execute), and the image with each section's own
 * rights (text read-only, the rest no-execute), then loads it. The low
 * identity map goes with the loader's tables, so page 0 faults. Until then
 * the new tables are written through the loader's window at KERNEL_VBASE,
 * which is why their pages come from below 1 GiB (pmm_boot_alloc); after,
 * every table is reached through the direct map, in any address space, so
 * nothing needs the recursive mapping or the temporary slot of x86-32.
 *
 * Redox and Haiku's x86-64 paging work this way
 * (src/system/kernel/arch/x86/paging/64bit).
 */

#include "arch/x86_64/pagetable.h"
#include "boot/cxbi.h"
#include "memman/pagedb.h"
#include "pmm.h"
#include "panic.h"

#define LOADER_WINDOW_END  0x40000000ull   /* long.asm maps this much at KERNEL_VBASE */
#define CR0_WP             (1ull << 16)

extern uint8_t __text_start[], __rodata_start[], __data_start[], __kernel_end[];

static paddr_t kernel_root;
static int     live;   /* on our own tables: reach them through the direct map */

static uint64_t *table_at(paddr_t phys) {
    return (uint64_t *)(uintptr_t)((live ? DIRECT_MAP_BASE : KERNEL_VBASE) + phys);
}

static paddr_t new_table(void) {
    paddr_t p = live ? pmm_alloc() : pmm_boot_alloc(LOADER_WINDOW_END);
    if (!p) {
        if (!live) kpanic("paging_init: no room below 1 GiB for the page tables");
        return 0;
    }
    uint64_t *t = table_at(p);
    for (unsigned i = 0; i < PT64_ENTRIES; i++) t[i] = 0;
    return p;
}

static inline void invlpg(uint64_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

/* The page-table entry for `virt` under `root`, making the tables on the way
   when `create`. 0 when a table is missing and not to be made, when none can
   be had, or when a large page covers `virt` (there is no 4 KiB entry).
   Tables made for a ring-3 mapping are reachable from ring 3 and the leaf
   decides the rest. Only paging_map_user makes tables below the kernel half,
   so a user walk never meets one made without the user bit. */
static uint64_t *walk(paddr_t root, uint64_t virt, int create, int user) {
    static const unsigned shift[3] = { 39, 30, 21 };
    uint64_t *t = table_at(root);
    for (unsigned l = 0; l < 3; l++) {
        uint64_t *e = &t[(virt >> shift[l]) & 511u];
        if (!(*e & PTE64_PRESENT)) {
            if (!create) return 0;
            paddr_t n = new_table();
            if (!n) return 0;
            *e = pte64_table(n, user);
        } else if (*e & PTE64_LARGE) {
            return 0;
        }
        t = table_at(*e & PTE64_ADDR);
    }
    return &t[PT_INDEX(virt)];
}

/* A 2 MiB page of the direct map, unless 4 KiB pages already map part of it:
   1 if it is mapped now, 0 if the caller must use 4 KiB pages. */
static int map_large(uint64_t virt, paddr_t phys) {
    uint64_t *t = table_at(kernel_root);
    for (unsigned shift = 39; shift > 21; shift -= 9) {
        uint64_t *e = &t[(virt >> shift) & 511u];
        if (!(*e & PTE64_PRESENT)) *e = pte64_table(new_table(), 0);
        t = table_at(*e & PTE64_ADDR);
    }
    uint64_t *pde = &t[PD_INDEX(virt)];
    if (*pde & PTE64_PRESENT) return (*pde & PTE64_LARGE) != 0;
    *pde = pde64_large(phys, VM_WRITE);
    return 1;
}

/* Physical [lo, hi) at DIRECT_MAP_BASE + lo: writable, no-execute, 2 MiB
   pages where a whole one fits in the range. Only whole pages inside it: a
   page shared with a hole is left out, so the direct map never covers device
   memory with a cacheable mapping. */
static void map_direct(uint64_t lo, uint64_t hi) {
    lo = (lo + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    hi &= ~(uint64_t)(PAGE_SIZE - 1);
    if (hi > DIRECT_MAP_SIZE) hi = DIRECT_MAP_SIZE;
    while (lo < hi) {
        uint64_t virt = DIRECT_MAP_BASE + lo;
        if (!(lo & (PAGE_2M - 1)) && hi - lo >= PAGE_2M && map_large(virt, lo)) {
            lo += PAGE_2M;
            continue;
        }
        uint64_t *e = walk(kernel_root, virt, 1, 0);
        if (e) *e = pte64_encode(lo, VM_WRITE, 0);   /* 0: a large page has it already */
        lo += PAGE_SIZE;
    }
}

static int is_ram(uint32_t type) {
    return type == CXBI_MEM_USABLE || type == CXBI_MEM_ACPI_RECLAIM ||
           type == CXBI_MEM_ACPI_NVS || type == CXBI_MEM_LOADER;
}

void paging_init(void) {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0 | CR0_WP) : "memory");

    kernel_root = new_table();
    uint64_t *pml4 = table_at(kernel_root);
    for (unsigned i = PML4_KERNEL_LO; i < PT64_ENTRIES; i++)
        pml4[i] = pte64_table(new_table(), 0);

    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (is_ram(e->type)) map_direct(e->base, e->base + e->length);
    }

    /* the image, section by section (linker.ld aligns each to a page) */
    uint64_t end = ((uint64_t)(uintptr_t)__kernel_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t v = (uint64_t)(uintptr_t)__text_start; v < end; v += PAGE_SIZE) {
        uint32_t vm = v < (uint64_t)(uintptr_t)__rodata_start ? VM_EXEC
                    : v < (uint64_t)(uintptr_t)__data_start   ? 0
                    : VM_WRITE;
        *walk(kernel_root, v, 1, 0) = pte64_encode(v - KERNEL_VBASE, vm, 0);
    }

    addr_space_switch(kernel_root);
    live = 1;
}

/* ---- mappings ------------------------------------------------------------------ */

static int in_kernel_regions(uint64_t virt) {
    return virt >= DIRECT_MAP_BASE + DIRECT_MAP_SIZE && virt < KERNEL_VBASE;
}

/* Anywhere in the kernel half but the direct map and the image, which are
   made once at boot. A kernel mapping in the user half would exist in one
   address space only, and one in the direct map would split its large pages:
   both are bugs in the caller. */
void paging_map_kernel(uintptr_t virt, paddr_t phys, uint32_t vm) {
    if (vm & ~VM_ALL) kpanic("paging_map_kernel: not a VM_* flag");
    if (!in_kernel_regions(virt)) kpanic("paging_map_kernel: outside the kernel's mappable regions");
    uint64_t *e = walk(addr_space_current(), virt, 1, 0);
    if (!e) return;   /* out of memory: the page stays unmapped */
    *e = pte64_encode(phys, vm, 0);
    invlpg(virt);
}

/* At PHYS_WINDOW_BASE + phys, so the same frame always comes back at the
   same place and a range can be mapped again, longer. */
volatile void *paging_map_phys(paddr_t phys, uint32_t len, uint32_t vm) {
    paddr_t last = phys + len - 1;
    if (len == 0 || last < phys || last >= PHYS_WINDOW_SIZE) return 0;
    for (paddr_t a = phys & ~(paddr_t)0xFFF; a <= last; a += PAGE_SIZE) {
        paging_map_kernel(PHYS_WINDOW_BASE + a, a, vm);
        if (!paging_get_phys(PHYS_WINDOW_BASE + a)) return 0;
    }
    return (volatile void *)(uintptr_t)(PHYS_WINDOW_BASE + phys);
}

/* Returns -1 for an address outside the user half or a flag that is not
   VM_*, and also, unlike x86-32, when no page is left for a table. */
int paging_map_user(uintptr_t virt, paddr_t phys, uint32_t vm) {
    if (virt >= USER_VLIMIT || (vm & ~VM_ALL)) return -1;
    uint64_t *e = walk(addr_space_current(), virt, 1, 1);
    if (!e) return -1;
    *e = pte64_encode(phys, vm, 1);
    invlpg(virt);
    return 0;
}

void paging_unmap(uintptr_t virt) {
    uint64_t *e = walk(addr_space_current(), virt, 0, 0);
    if (!e) return;
    *e = 0;
    invlpg(virt);
}

/* The leaf entry mapping `virt` in the active space, and the size it maps; 0
   if none. Every level must carry `need`, since the CPU takes the strictest
   of them: a writable page under a read-only table is not writable. */
static uint64_t leaf(uint64_t virt, uint64_t need, uint64_t *size) {
    uint64_t *t = table_at(addr_space_current());
    for (unsigned shift = 39; ; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511u];
        if ((e & (need | PTE64_PRESENT)) != (need | PTE64_PRESENT)) return 0;
        if (shift == 12 || (shift < 39 && (e & PTE64_LARGE))) {
            *size = 1ull << shift;
            return e;
        }
        t = table_at(e & PTE64_ADDR);
    }
}

paddr_t paging_get_phys(uintptr_t virt) {
    uint64_t size, e = leaf(virt, 0, &size);
    if (!e) return 0;
    return (e & PTE64_ADDR & ~(size - 1)) | (virt & (size - 1));
}

int paging_is_user(uintptr_t virt) {
    uint64_t size;
    return leaf(virt, PTE64_USER, &size) != 0;
}

int paging_is_user_writable(uintptr_t virt) {
    uint64_t size;
    return leaf(virt, PTE64_USER | PTE64_WRITE, &size) != 0;
}

/* Every frame is in the direct map already: nothing to map or undo. */
void *paging_temp_map(paddr_t phys) {
    return (void *)(uintptr_t)(DIRECT_MAP_BASE + (phys & ~(paddr_t)0xFFF));
}

void paging_temp_unmap(void) { }

int paging_kernel_obeys_readonly(void) {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & CR0_WP) != 0;
}

/* ---- address spaces --------------------------------------------------------------- */

/* Nothing to register: the kernel half is complete, so no space ever needs
   an entry pushed into it. */
void addr_space_init(void) { }

int addr_space_create(paddr_t *root) {
    if (!root) return -1;
    paddr_t p = pmm_alloc();
    if (!p) return -1;
    pml4_init(table_at(p), table_at(kernel_root));
    *root = p;
    return 0;
}

/* Free a user-half table and everything under it: the frames its 4 KiB
   entries map, as x86-32 does, and the tables. `level` 1 is a PDPT. */
static void free_tree(paddr_t table, unsigned level) {
    uint64_t *t = table_at(table);
    for (unsigned i = 0; i < PT64_ENTRIES; i++) {
        uint64_t e = t[i];
        if (!(e & PTE64_PRESENT)) continue;
        if (level == 3)                pmm_free(e & PTE64_ADDR);
        else if (!(e & PTE64_LARGE))   free_tree(e & PTE64_ADDR, level + 1);
    }
    pmm_free(table);
}

void addr_space_release(paddr_t root) {
    if (!root) return;
    if (root == addr_space_current()) kpanic("addr_space_release: the space is in use");
    if (root == kernel_root) kpanic("addr_space_release: the kernel's own space");
    uint64_t *pml4 = table_at(root);
    for (unsigned i = 0; i < PML4_KERNEL_LO; i++) {
        if (pml4[i] & PTE64_PRESENT) free_tree(pml4[i] & PTE64_ADDR, 1);
        pml4[i] = 0;
    }
    pmm_free(root);
}

paddr_t addr_space_kernel(void) { return kernel_root; }

paddr_t addr_space_current(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE64_ADDR;
}

void addr_space_switch(paddr_t root) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(root) : "memory");
}
