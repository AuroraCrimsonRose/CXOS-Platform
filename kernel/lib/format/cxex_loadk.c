/* /kernel/lib/format/cxex_loadk.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Kernel binding for cxex_load: supply the get_page/map_page ops backed by the
 * PMM and the pager. Kept separate from cxex_load.c so the loader logic stays
 * pure (host-testable with mock ops).
 *
 * Frames are written through the pager's temp-map window, so any physical frame
 * works (not just boot-mapped low RAM). Mapping targets the CURRENT address
 * space: the intended flow is addr_space_create -> addr_space_switch -> cxex_load
 * with these ops, so "current" IS the new space, and the image lands in it.
 */

#include "cxex_load.h"
#include "pmm.h"
#include "paging.h"

/* The user half is everything below the higher-half kernel. Defined locally, as
   the other users of this constant do (usermode.c, paging.c, addr_space.c);
   giving it one home is engineering review §13's "localise x86 assumptions". */
#define KERNEL_VBASE 0xC0000000u

static void *k_get_page(void *ctx, uint32_t *out_phys) {
    (void)ctx;
    void *phys = pmm_alloc();
    if (!phys) return NULL;
    /* reach the frame through the temp-map window (works for any frame, not just
       boot-mapped low RAM); the loader writes the page's contents through it. */
    uint8_t *v = (uint8_t *)paging_temp_map((uint32_t)phys);
    for (uint32_t i = 0; i < 4096u; i++) v[i] = 0;     /* arrive zeroed (BSS) */
    *out_phys = (uint32_t)phys;
    return v;
}

static int k_map_page(void *ctx, uint32_t virt, uint32_t phys, uint32_t prot) {
    (void)ctx;

    /* Refused here as well as in the loader's validation pass. The loader proves
       no section reaches KERNEL_VBASE before it maps anything, so this can only
       fire if that pass is bypassed or wrong - which is exactly when it matters.
       Mapping a kernel address with PAGE_USER would hand ring 3 the kernel
       (security review §1, §8); one duplicated comparison is a cheap price for
       the mapping call being unable to express it. */
    if (virt >= KERNEL_VBASE) return -1;

    uint32_t f = PAGE_PRESENT | PAGE_USER;             /* loaded image is ring 3 */
    if (prot & CXEX_PROT_WRITE) f |= PAGE_WRITE;
    /* x86 (non-PAE) has no per-page execute bit; EXEC is implicit. */
    paging_map(virt, phys, f);
    return 0;
}

const struct cxex_load_ops cxex_kernel_load_ops = {
    0,              /* ctx: current space for now */
    KERNEL_VBASE,   /* va_limit: the user half is everything below the kernel */
    k_get_page,
    k_map_page
};