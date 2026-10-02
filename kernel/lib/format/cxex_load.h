/* /kernel/lib/format/cxex_load.h */
/* Aurora Tejeda / CATX Systems */
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

    /* One past the highest virtual address this image may occupy. Every byte of
       every section must fall below it, so a loaded image can never name a
       kernel address (security review §1) - the loader used to pass the image's
       virt_addr straight to map_page, which happily mapped 0xC0000000 and up
       with the USER bit set.

       It lives here rather than as a constant in the loader because the loader
       is deliberately free of kernel headers, so it stays host-testable with
       mock ops. The kernel binding sets it to KERNEL_VBASE; a test can set it to
       whatever it wants to exercise. Zero means "no limit" and is refused, so a
       caller that forgets it fails loudly rather than silently losing the check. */
    uint32_t va_limit;

    /* Allocate one page, ZEROED, for the target space. Return a pointer the
       loader can write the page's contents through, and set *out_phys to the
       physical address that map_page should map. NULL on failure. */
    void *(*get_page)(void *ctx, uint32_t *out_phys);

    /* Map physical page `phys` at page-aligned target virtual address `virt`
       with the given prot bits. Return 0 on success, non-zero on failure. */
    int (*map_page)(void *ctx, uint32_t virt, uint32_t phys, uint32_t prot);

    /* Charge `pages` 4 KB pages to whoever owns this load. Called ONCE, with
       the whole bill, after validation and before the first get_page - so an
       image that cannot be afforded costs no frames at all, where charging as
       the pages arrived would leave the refused load holding everything it got
       before the ceiling (security review §3). Return 0 to allow, non-zero to
       refuse. May be NULL, which is "not charged to anything": that is what the
       host and ktest mock ops use, and what CXEX_LOAD_MAX_PAGES still bounds. */
    int (*charge_pages)(void *ctx, uint32_t pages);
};

enum cxex_load_result {
    CXEX_LOAD_OK          =  0,
    CXEX_LOAD_BAD_FORMAT  = -1,   /* header didn't parse / bad args */
    CXEX_LOAD_NOT_EXEC    = -2,   /* image lacks the EXECUTABLE flag */
    CXEX_LOAD_BAD_SECTION = -3,   /* a section entry failed to parse */
    CXEX_LOAD_OOB         = -4,   /* a section's file bytes lie outside the image */
    CXEX_LOAD_NOMEM       = -5,   /* get_page failed */
    CXEX_LOAD_MAP_FAIL    = -6,   /* map_page failed */
    CXEX_LOAD_BAD_RANGE   = -7,   /* a section leaves the user half, or its range wraps */
    CXEX_LOAD_WX          = -8,   /* a section is both writable and executable */
    CXEX_LOAD_UNSIGNED    = -9,   /* a section's bytes lie outside the signed range */
    CXEX_LOAD_TOO_BIG     = -10,  /* the image asks for more pages than are allowed */
    CXEX_LOAD_QUOTA       = -11   /* the owner's memory quota has no room for it */
};

/* Ceiling on how many pages one image may map, whoever is loading it. A real
   .xoex is a handful; the cap stops a header claiming gigabytes from draining
   the PMM one frame at a time before anything notices (security review §3).
   It is the floor of the two limits, not the only one: charge_pages puts the
   same image against the owning process's quota, which is far smaller. */
#define CXEX_LOAD_MAX_PAGES 16384u   /* 64 MB */

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