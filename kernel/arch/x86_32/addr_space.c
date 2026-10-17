// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/addr_space.c */
/* Aurora Tejeda / CATX Systems */
/* Address spaces (arch/paging.h) on 32-bit x86, and keeping the shared kernel
   half in step across them. The directory layout is in pagetable.h. */

#include "arch/x86_32/pagetable.h"
#include "pmm.h"
#include "sync/spinlock.h"
#include "panic.h"

/* the kernel's page directory, built by entry.asm (a kernel symbol => virtual).
   Its physical address is (virtual - KERNEL_VBASE). */
extern uint32_t boot_page_dir[];

/* Registry of live spaces' page-directory physical addresses, so a newly
   created kernel-half PDE can be pushed into every space. Small fixed table;
   index 0 is the kernel space. */
#define MAX_SPACES 64
static paddr_t g_spaces[MAX_SPACES];
static int     g_space_count = 0;

static void registry_add(paddr_t pd_phys) {
    for (int i = 0; i < g_space_count; i++) if (g_spaces[i] == pd_phys) return;
    if (g_space_count < MAX_SPACES) g_spaces[g_space_count++] = pd_phys;
}

static void registry_remove(paddr_t pd_phys) {
    for (int i = 0; i < g_space_count; i++) {
        if (g_spaces[i] == pd_phys) {
            g_spaces[i] = g_spaces[--g_space_count];
            return;
        }
    }
}

/* A new shared-kernel-half PDE was created; write it into every live space's
   directory so they all see the new kernel region. Reaches each directory
   through the temp-map (works for any frame). */
void addr_space_propagate_pde(uint32_t index, uint32_t value) {
    if (index < PD_KERNEL_LO || index > PD_KERNEL_HI) return;
    for (int i = 0; i < g_space_count; i++) {
        uint32_t *pd = (uint32_t *)paging_temp_map(g_spaces[i]);
        pd[index] = value;
        paging_temp_unmap();
    }
}

void addr_space_init(void) {
    g_space_count = 0;
    registry_add(addr_space_kernel());   /* kernel space = index 0 */
    paging_propagate_on();
}

int addr_space_create(paddr_t *root) {
    if (!root) return -1;
    void *pd_phys = pmm_alloc();                  /* one 4 KB frame for the PD */
    if (!pd_phys) return -1;
    /* reach the frame through the temp-map to fill it in (works for any frame,
       not just boot-mapped low RAM). */
    uint32_t *pd = (uint32_t *)paging_temp_map((paddr_t)pd_phys);
    pd_init(pd, boot_page_dir, (paddr_t)pd_phys);
    paging_temp_unmap();
    *root = (paddr_t)pd_phys;
    registry_add(*root);
    return 0;
}

/* Free the user half of the ACTIVE space: every user frame and page-table
   frame below the kernel base (PDE 0..767), via the recursive directory. The
   kernel half is shared and left alone. */
#define AS_RECURSIVE_PD     ((volatile uint32_t *)0xFFFFF000u)
#define AS_RECURSIVE_PT(i)  ((volatile uint32_t *)(0xFFC00000u + ((uint32_t)(i) << 12)))

static void reclaim_user(void) {
    for (uint32_t i = 0; i < PD_KERNEL_LO; i++) {   /* user half: 0..767 */
        uint32_t pde = AS_RECURSIVE_PD[i];
        if (!(pde & PTE_PRESENT)) continue;

        volatile uint32_t *pt = AS_RECURSIVE_PT(i);
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            uint32_t pte = pt[j];
            if (pte & PTE_PRESENT) pmm_free((void *)(pte & PTE_ADDR_MASK));  /* user frame */
        }
        AS_RECURSIVE_PD[i] = 0;                  /* unmap the PT (recursive view gone) */
        pmm_free((void *)(pde & PTE_ADDR_MASK)); /* then free the page-table frame */
    }
}

void addr_space_release(paddr_t root) {
    if (!root) return;
    uint32_t f = spin_irq_save();
    paddr_t was = addr_space_current();
    /* Freeing the directory in use would leave the CPU walking a free frame. */
    if (was == root) kpanic("addr_space_release: the space is in use");
    /* Its own recursive slot is the only view of its page tables, so the walk
       runs inside it. The kernel code, stack and heap doing the walk are in the
       shared half, mapped identically in every space. */
    addr_space_switch(root);
    reclaim_user();
    addr_space_switch(was);   /* drops its TLB entries too */
    spin_irq_restore(f);
    registry_remove(root);
    pmm_free((void *)root);   /* the directory itself */
}

paddr_t addr_space_kernel(void) {
    return (paddr_t)boot_page_dir - KERNEL_VBASE;
}

paddr_t addr_space_current(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE_ADDR_MASK;
}

void addr_space_switch(paddr_t root) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(root) : "memory");
}
