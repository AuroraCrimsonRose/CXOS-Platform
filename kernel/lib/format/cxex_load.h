/* /kernel/lib/format/cxex_load.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXEX runtime loader: place a parsed CXEX image's sections into an address
 * space and return its entry point. The C analogue of boot/cxexload.asm (which
 * places the kernel at boot); this one runs in the kernel to load executives
 * and apps (.xoex / .xuex) at runtime.
 *
 * It is TYPE-AGNOSTIC: it just lays out sections. Verifying the signature
 * (cxex_verify) and deciding what the image is ALLOWED to do (policy: ring,
 * capability tier) are separate steps the caller performs around it.
 *
 * The actual page allocation + mapping is supplied by the caller through
 * cxex_load_ops, so the placement logic here is independent of the VMM and can
 * target the current space today or a per-process space later, unchanged.
 */

#ifndef CXEX_LOAD_H
#define CXEX_LOAD_H

#include <stdint.h>
#include <stddef.h>

/* protection bits passed to ops->map_page (translated from section R/W/X) */
#define CXEX_PROT_READ   (1u << 0)
#define CXEX_PROT_WRITE  (1u << 1)
#define CXEX_PROT_EXEC   (1u << 2)
#define CXEX_PROT_USER   (1u << 3)   /* ring-3 accessible (loaded images are ring 3) */

struct cxex_load_ops {
    void *ctx;   /* opaque target (e.g. an address space); passed back to the ops */

    /* Allocate one page, ZEROED, for the target space. Return a pointer the
       loader can write the page's contents through, and set *out_phys to the
       physical address that map_page should map. NULL on failure. */
    void *(*get_page)(void *ctx, uint32_t *out_phys);

    /* Map physical page `phys` at page-aligned target virtual address `virt`
       with the given prot bits. Return 0 on success, non-zero on failure. */
    int (*map_page)(void *ctx, uint32_t virt, uint32_t phys, uint32_t prot);
};

enum cxex_load_result {
    CXEX_LOAD_OK          =  0,
    CXEX_LOAD_BAD_FORMAT  = -1,   /* header didn't parse / bad args */
    CXEX_LOAD_NOT_EXEC    = -2,   /* image lacks the EXECUTABLE flag */
    CXEX_LOAD_BAD_SECTION = -3,   /* a section entry failed to parse */
    CXEX_LOAD_OOB         = -4,   /* a section's file bytes lie outside the image */
    CXEX_LOAD_NOMEM       = -5,   /* get_page failed */
    CXEX_LOAD_MAP_FAIL    = -6    /* map_page failed */
};

/* Load `file` (a complete CXEX image; verify it first) into the space described
   by `ops`. On success returns CXEX_LOAD_OK and writes the entry-point virtual
   address to *entry_out. Assumes sections are page-aligned and non-overlapping
   in their pages (true for mkcxes output from ELF PT_LOAD). */
int cxex_load(const uint8_t *file, size_t len,
              const struct cxex_load_ops *ops, uint32_t *entry_out);

const char *cxex_load_strerror(int result);

/* Kernel-side ops (cxex_loadk.c): allocate PMM frames and map them into the
   CURRENT address space. Pass &cxex_kernel_load_ops to cxex_load. When per-
   process address spaces land, this is replaced by ops that target a specific
   space. */
extern const struct cxex_load_ops cxex_kernel_load_ops;

#endif