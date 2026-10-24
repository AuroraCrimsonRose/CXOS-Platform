// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/xhci_spad.c */
/* Aurora Tejeda / CATX Systems */
/* xHCI scratchpad buffers, from the page allocator (see xhci_spad.h). The
   pages need no kernel mapping of their own: only the controller uses them,
   so each is zeroed through the temporary mapping and left unmapped. */

#include "xhci_spad.h"
#include "pmm.h"

#define ENTRIES_PER_PAGE (PAGE_SIZE / sizeof(uint64_t))

static uint32_t array_pages(uint32_t count) {
    return (count + ENTRIES_PER_PAGE - 1) / ENTRIES_PER_PAGE;
}

static void zero_page(paddr_t p) {
    volatile uint32_t *v = (volatile uint32_t *)paging_temp_map(p);
    for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) v[i] = 0;
    paging_temp_unmap();
}

static void set_entry(paddr_t array, uint32_t i, uint64_t value) {
    volatile uint64_t *v = (volatile uint64_t *)
        paging_temp_map(array + (i / ENTRIES_PER_PAGE) * PAGE_SIZE);
    v[i % ENTRIES_PER_PAGE] = value;
    paging_temp_unmap();
}

uint64_t xhci_spad_entry(paddr_t array, uint32_t i) {
    volatile uint64_t *v = (volatile uint64_t *)
        paging_temp_map(array + (i / ENTRIES_PER_PAGE) * PAGE_SIZE);
    uint64_t value = v[i % ENTRIES_PER_PAGE];
    paging_temp_unmap();
    return value;
}

/* Free the first `filled` pages and the array, sized for `count` entries. */
static void release(paddr_t array, uint32_t filled, uint32_t count) {
    for (uint32_t i = 0; i < filled; i++)
        pmm_free(xhci_spad_entry(array, i));
    pmm_free_pages(array, array_pages(count));
}

int xhci_spad_build(uint32_t count, paddr_t *array) {
    *array = 0;
    if (count == 0) return 0;
    if (count > XHCI_SPAD_MAX) return -1;

    uint32_t ap = array_pages(count);
    paddr_t arr = (paddr_t)(uintptr_t)pmm_alloc_pages(ap);
    if (!arr) return -1;
    for (uint32_t k = 0; k < ap; k++) zero_page(arr + k * PAGE_SIZE);

    for (uint32_t i = 0; i < count; i++) {
        paddr_t pg = (paddr_t)(uintptr_t)pmm_alloc();
        if (!pg) { release(arr, i, count); return -1; }
        zero_page(pg);
        set_entry(arr, i, pg);
    }
    *array = arr;
    return 0;
}

void xhci_spad_release(paddr_t array, uint32_t count) {
    if (array) release(array, count, count);
}
