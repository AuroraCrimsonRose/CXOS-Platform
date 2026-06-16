/* /CXLite/kernel/memman/paging.c */
/* Aurora Tejeda */
/* x86 32-bit paging: 2-level tables, 4 KB pages. */

#include "paging.h"
#include "fb.h"
#include "pmm.h"

extern uint8_t __bss_end[];   /* end of kernel image, from linker */

/* The page directory: 1024 entries, each pointing to a page table (or unused).
   Must be page-aligned. We allocate it from the PMM in paging_init. */
static uint32_t *page_directory = 0;

/* extract directory / table indices from a virtual address */
#define PD_INDEX(v)  ((v) >> 22)            /* top 10 bits */
#define PT_INDEX(v)  (((v) >> 12) & 0x3FF)  /* next 10 bits */

/* get the page table for a virtual address, allocating one if needed
   (and `create` is set). Returns physical/linear pointer to the table. */
static uint32_t *get_table(uint32_t virt, int create) {
    uint32_t pd_i = PD_INDEX(virt);
    if (!(page_directory[pd_i] & PAGE_PRESENT)) {
        if (!create) return 0;
        /* allocate a fresh page table from the PMM */
        uint32_t *table = (uint32_t *)pmm_alloc();
        if (!table) return 0;
        /* zero it (no mappings yet) */
        for (int i = 0; i < PAGE_ENTRIES; i++) table[i] = 0;
        /* install into the directory: present + writable (+user later) */
        page_directory[pd_i] = ((uint32_t)table) | PAGE_PRESENT | PAGE_WRITE;
    }
    /* identity-mapped world: the table's physical address is its linear address */
    return (uint32_t *)(page_directory[pd_i] & ~0xFFFu);
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t *table = get_table(virt, 1);
    if (!table) return;
    table[PT_INDEX(virt)] = (phys & ~0xFFFu) | (flags & 0xFFF) | PAGE_PRESENT;

    /* The CPU ANDs the privilege/permission bits across BOTH paging levels: a
       page is only user-accessible (or writable) if the page-directory entry
       ALSO grants it. The PDE may already exist (e.g. boot identity map) as
       supervisor-only, so OR the requested USER/WRITE bits into it too -
       otherwise a ring-3 access faults even though the PTE allows it. */
    uint32_t pd_i = PD_INDEX(virt);
    if (flags & PAGE_USER)  page_directory[pd_i] |= PAGE_USER;
    if (flags & PAGE_WRITE) page_directory[pd_i] |= PAGE_WRITE;

    /* flush this page from the TLB so the new mapping takes effect */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void paging_unmap(uint32_t virt) {
    uint32_t *table = get_table(virt, 0);
    if (!table) return;
    table[PT_INDEX(virt)] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

uint32_t paging_get_phys(uint32_t virt) {
    uint32_t *table = get_table(virt, 0);
    if (!table) return 0;
    uint32_t entry = table[PT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    return (entry & ~0xFFFu) | (virt & 0xFFFu);
}

void paging_init(void) {
    /* 1. allocate the page directory from the PMM and zero it */
    page_directory = (uint32_t *)pmm_alloc();
    for (int i = 0; i < PAGE_ENTRIES; i++) page_directory[i] = 0;

    /* 2. identity-map from 0 up to a generous ceiling. This must cover not just
          the kernel + PMM bitmap, but enough low RAM that page tables (allocated
          low-first by the PMM) and early heap growth stay inside mapped memory.
          A page table allocated above the map would fault when get_table() zeroes
          it (and we can't map it first without recursing). Mapping a fixed low
          region avoids that: the PMM scans from page 0, so these low pages back
          page tables and early allocations. Higher heap pages are mapped on
          demand in heap_grow(). Bochs(2GB) vs QEMU(4GB) put free RAM in different
          places, so a too-small map faults on one but not the other - a fixed
          generous floor fixes both. */
    uint32_t bss_ceiling = ((uint32_t)__bss_end) + 0x400000;   /* kernel + 4MB */
    uint32_t min_ceiling = 0x08000000;                          /* 128 MB floor */
    uint32_t ceiling = (bss_ceiling > min_ceiling) ? bss_ceiling : min_ceiling;
    ceiling = (ceiling + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t addr = 0; addr < ceiling; addr += PAGE_SIZE) {
        paging_map(addr, addr, PAGE_WRITE);   /* present + writable, kernel-only */
    }

    /* also identity-map VGA memory region (0xB8000) - it's under 1 MB so it's
       already covered by the loop above, but make it explicit for clarity:
       (no-op if already mapped) */
    paging_map(0xB8000, 0xB8000, PAGE_WRITE);

    /* If a graphics framebuffer is active, its physical address is typically
       HIGH (e.g. 0xFD000000) and far outside the identity range above. Map it
       here, into the page tables we're about to load, so framebuffer writes
       keep working once paging is enabled. (fb_init reads the VBE info but
       leaves mapping to us, since it may run before paging exists.) */
    uint32_t fb_phys, fb_size;
    if (fb_get_region(&fb_phys, &fb_size)) {
        uint32_t start = fb_phys & ~(PAGE_SIZE - 1);
        uint32_t end   = (fb_phys + fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint32_t a = start; a < end; a += PAGE_SIZE)
            paging_map(a, a, PAGE_WRITE);
    }

    /* 3. load CR3 with the page directory's physical address */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));

    /* 4. enable paging: set CR0.PG (bit 31). After this instruction every
          address is translated - the identity map must be correct or we
          triple-fault here. */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}