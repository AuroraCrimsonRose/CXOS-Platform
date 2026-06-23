/* /CXK/kernel/memman/addr_space.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* Per-process address spaces + shared-kernel-half sync. See addr_space.h. */

#include "addr_space.h"
#include "pmm.h"
#include "paging.h"

#define KERNEL_VBASE  0xC0000000u

/* the kernel's page directory, built by kernel.asm (a kernel symbol => virtual).
   Its physical address is (virtual - KERNEL_VBASE). */
extern uint32_t boot_page_dir[];

/* Registry of live spaces' page-directory physical addresses, so a newly
   created kernel-half PDE can be pushed into every space. Small fixed table;
   index 0 is the kernel space. */
#define MAX_SPACES 64
static uint32_t g_spaces[MAX_SPACES];
static int      g_space_count = 0;

static void registry_add(uint32_t pd_phys) {
    for (int i = 0; i < g_space_count; i++) if (g_spaces[i] == pd_phys) return;
    if (g_space_count < MAX_SPACES) g_spaces[g_space_count++] = pd_phys;
}

static void registry_remove(uint32_t pd_phys) {
    for (int i = 0; i < g_space_count; i++) {
        if (g_spaces[i] == pd_phys) {
            g_spaces[i] = g_spaces[--g_space_count];
            return;
        }
    }
}

void addr_space_init_pd(uint32_t *new_pd, const uint32_t *kernel_pd,
                        uint32_t new_pd_phys) {
    for (uint32_t i = 0; i < ADDR_SPACE_KERNEL_PDE_LO; i++)
        new_pd[i] = 0;                                         /* private user half */
    for (uint32_t i = ADDR_SPACE_KERNEL_PDE_LO; i <= ADDR_SPACE_KERNEL_PDE_HI; i++)
        new_pd[i] = kernel_pd[i];                              /* shared kernel half */
    new_pd[ADDR_SPACE_RECURSIVE_PDE] =
        (new_pd_phys & ~0xFFFu) | PAGE_PRESENT | PAGE_WRITE;   /* own recursive slot */
}

/* Hook: a new shared-kernel-half PDE was created; write it into every live
   space's directory so they all see the new kernel region. Reaches each
   directory through the temp-map (works for any frame). */
void addr_space_propagate_pde(uint32_t index, uint32_t value) {
    if (index < ADDR_SPACE_KERNEL_PDE_LO || index > ADDR_SPACE_KERNEL_PDE_HI) return;
    for (int i = 0; i < g_space_count; i++) {
        uint32_t *pd = (uint32_t *)paging_temp_map(g_spaces[i]);
        pd[index] = value;
        paging_temp_unmap();
    }
}

/* Register the kernel space and arm the PDE-creation hook. Call once, after
   paging_init, before creating any other space. */
void addr_space_init(void) {
    g_space_count = 0;
    registry_add((uint32_t)boot_page_dir - KERNEL_VBASE);   /* kernel space = index 0 */
    paging_set_pde_hook(addr_space_propagate_pde);
}

int addr_space_create(struct addr_space *out) {
    if (!out) return -1;
    void *pd_phys = pmm_alloc();                  /* one 4 KB frame for the PD */
    if (!pd_phys) return -1;
    /* reach the frame through the temp-map to fill it in (works for any frame,
       not just boot-mapped low RAM). */
    uint32_t *pd = (uint32_t *)paging_temp_map((uint32_t)pd_phys);
    addr_space_init_pd(pd, boot_page_dir, (uint32_t)pd_phys);
    paging_temp_unmap();
    out->pd_phys = (uint32_t)pd_phys;
    registry_add((uint32_t)pd_phys);
    return 0;
}

/* Reclaim the user half of the CURRENTLY ACTIVE address space: free every user
   frame and page-table frame mapped below the kernel base (PDE 0..767), via the
   recursive page-directory mapping. The kernel half (PDE 768+) is shared across
   all spaces and is left untouched. Call this while the space is active and the
   process has exited (its user pages are no longer in use); the kernel code +
   stacks doing the reclaim live in the shared higher half, so freeing user
   frames underneath is safe. After this, switch to the kernel space and call
   addr_space_destroy() to free the page directory itself. */
#define AS_RECURSIVE_PD     ((volatile uint32_t *)0xFFFFF000u)
#define AS_RECURSIVE_PT(i)  ((volatile uint32_t *)(0xFFC00000u + ((uint32_t)(i) << 12)))

void addr_space_reclaim_user(void) {
    for (uint32_t i = 0; i < ADDR_SPACE_KERNEL_PDE_LO; i++) {   /* user half: 0..767 */
        uint32_t pde = AS_RECURSIVE_PD[i];
        if (!(pde & PAGE_PRESENT)) continue;

        volatile uint32_t *pt = AS_RECURSIVE_PT(i);
        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t pte = pt[j];
            if (pte & PAGE_PRESENT) pmm_free((void *)(pte & ~0xFFFu));  /* user frame */
        }
        AS_RECURSIVE_PD[i] = 0;                  /* unmap the PT (recursive view gone) */
        pmm_free((void *)(pde & ~0xFFFu));       /* then free the page-table frame */
    }
}

void addr_space_destroy(const struct addr_space *s) {
    if (!s) return;
    registry_remove(s->pd_phys);
    pmm_free((void *)s->pd_phys);   /* free the page-directory frame itself.
                                       MUST NOT be the active space - switch away
                                       (e.g. to the kernel space) before calling. */
}

void addr_space_kernel(struct addr_space *out) {
    if (out) out->pd_phys = (uint32_t)boot_page_dir - KERNEL_VBASE;
}

void addr_space_switch(const struct addr_space *s) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(s->pd_phys) : "memory");
}