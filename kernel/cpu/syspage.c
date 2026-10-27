// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/syspage.c */
/* Aurora Tejeda / CATX Systems */
/* The system page: which slots hold a stub is the dispatch table's business,
   what a stub looks like is the architecture's (arch_syscall_stub). */

#include "syspage.h"
#include "systable.h"
#include "pmm.h"
#include "arch/usermode.h"

/* Every number the table can hold has a slot, and the slots fit the page. */
_Static_assert(SYSCALL_TABLE_SIZE * SYSPAGE_STUB_SIZE <= 4096u,
               "the syscall stubs must fit in one page");

void syspage_fill(uint8_t *page) {
    for (uint32_t i = 0; i < 4096u; i++) page[i] = 0xCC;   /* int3 */
    for (uint32_t n = 0; n < SYSCALL_TABLE_SIZE; n++)
        if (syscall_table[n]) arch_syscall_stub(page + n * SYSPAGE_STUB_SIZE, n);
}

int syspage_map(uintptr_t at, paddr_t *phys_out) {
    paddr_t phys = pmm_alloc();
    if (!phys) return -1;
    /* Filled through the temp-map window, as the loader fills an image page:
       the user mapping below is never writable, not even for a moment. */
    syspage_fill((uint8_t *)paging_temp_map(phys));
    if (paging_map_user(at, phys, VM_EXEC) != 0) { pmm_free(phys); return -1; }
    if (phys_out) *phys_out = phys;
    return 0;
}
