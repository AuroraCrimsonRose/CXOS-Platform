/* /CXLite/kernel/memman/paging.h */
/* Aurora Tejeda */
/*
 * Virtual memory / paging - x86 32-bit, 2-level (Page Directory -> Page Table),
 * 4 KB pages.
 *
 * v1: identity-maps the kernel and low memory, then enables the MMU, so
 * virtual == physical for existing memory and the transition is seamless.
 * Higher-half kernel mapping comes as the next step.
 */

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE        4096u
#define PAGE_ENTRIES     1024          /* entries per directory / table */

/* page flags (low bits of a PDE/PTE) */
#define PAGE_PRESENT     0x001
#define PAGE_WRITE       0x002
#define PAGE_USER        0x004         /* 1 = accessible from ring 3 */

/* set up identity mapping and enable paging. Call after pmm_init. */
void paging_init(void);

/* map one virtual page to one physical frame with the given flags.
   Both addresses must be page-aligned. */
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);

/* remove a mapping for a virtual page. */
void paging_unmap(uint32_t virt);

/* return the physical address a virtual address maps to, or 0 if unmapped. */
uint32_t paging_get_phys(uint32_t virt);
int      paging_is_user(uint32_t virt);   /* present + ring-3 accessible in active space */

/* temporarily map an arbitrary physical frame into a reserved scratch page so
   the kernel can read/write it; unmap when done. One frame at a time. */
void *paging_temp_map(uint32_t phys);
void  paging_temp_unmap(void);

/* register a hook called when paging_map creates a new shared-kernel-half PDE,
   so the address-space layer can propagate it into live spaces. */
void  paging_set_pde_hook(void (*fn)(uint32_t index, uint32_t value));

#endif