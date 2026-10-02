// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/format/cxex_loadk.c */
/* Aurora Tejeda / CATX Systems */
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
#include "vmregion.h"
#include "sched.h"

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

    /* paging_map_user refuses any address at or above KERNEL_VBASE and adds the
       USER bit itself, so this call cannot express a kernel mapping at all -
       which is the point of the split (security review §1, §8). The loader's
       validation pass has already proved the same thing; this is the layer that
       still holds if that pass is ever bypassed or wrong. */
    uint32_t f = PAGE_PRESENT;                         /* loaded image is ring 3 */
    if (prot & CXEX_PROT_WRITE) f |= PAGE_WRITE;
    /* x86 (non-PAE) has no per-page execute bit; EXEC is implicit. */
    return paging_map_user(virt, phys, f);
}

/* The image is charged to the process that is being built, which is the one
   running this code: cxex_load is called from proc_trampoline, on the new
   thread, after the switch into its own address space. That is the same "current"
   the mapping above relies on, so there is no second notion of the owner to get
   out of step with it. */
static int k_charge(void *ctx, uint32_t pages) {
    (void)ctx;
    /* pages <= CXEX_LOAD_MAX_PAGES (64 MB) by the time the loader calls this, so
       the shift into bytes cannot wrap - and vm_charge checks for a wrap anyway,
       which is the check that holds if the cap ever moves. */
    return vm_charge(thread_current_id(), pages * 4096u) == E_OK ? 0 : -1;
}

const struct cxex_load_ops cxex_kernel_load_ops = {
    0,              /* ctx: current space for now */
    KERNEL_VBASE,   /* va_limit: the user half is everything below the kernel */
    k_get_page,
    k_map_page,
    k_charge
};