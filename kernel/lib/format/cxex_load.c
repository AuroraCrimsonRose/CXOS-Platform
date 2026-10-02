// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/format/cxex_load.c */
/* Aurora Tejeda / CATX Systems */
/* CXEX runtime loader. See cxex_load.h. */

#include "cxex_load.h"
#include "cxex.h"

#define CXEX_PAGE_SIZE 4096u

static uint32_t prot_from_section(uint32_t sflags) {
    uint32_t p = CXEX_PROT_USER;          /* loaded images run in ring 3 */
    if (sflags & CXEX_SEC_READ)  p |= CXEX_PROT_READ;
    if (sflags & CXEX_SEC_WRITE) p |= CXEX_PROT_WRITE;
    if (sflags & CXEX_SEC_EXEC)  p |= CXEX_PROT_EXEC;
    return p;
}

/* Check one section without touching anything. Split out so the validation pass
   below and any future caller apply exactly the same rules. */
static int check_section(const struct cxex_section *s, const struct cxex_header *h,
                         size_t len, uint32_t va_limit, uint32_t *pages_out) {
    uint32_t fsz = (s->flags & CXEX_SEC_NOBITS) ? 0 : s->file_size;

    /* 64-bit throughout. These are untrusted 32-bit values whose sums wrap, and
       a wrapped sum compares small - which is precisely how a section that runs
       off the end of the image passes a check written in 32 bits. */
    if (fsz && (uint64_t)s->file_offset + fsz > len) return CXEX_LOAD_OOB;

    /* file_size may not exceed mem_size: the surplus has nowhere to live. */
    if (fsz > s->mem_size) return CXEX_LOAD_OOB;

    /* Every byte the signature covers ends at signature_offset. A section whose
       file bytes reach past it is data nobody signed, and the loader would map
       it anyway - the finding the plan adds in its own words. */
    if (h->signature_offset != 0 && fsz &&
        (uint64_t)s->file_offset + fsz > h->signature_offset)
        return CXEX_LOAD_UNSIGNED;

    /* The whole virtual range, including the zero-filled tail, must lie in the
       user half - and must not wrap to get there. */
    uint64_t va_end = (uint64_t)s->virt_addr + s->mem_size;
    if (va_end > va_limit) return CXEX_LOAD_BAD_RANGE;

    /* W^X. x86 without PAE has no per-page execute bit, so a writable page is
       executable whether or not anyone intended it; refusing the combination is
       the only place the rule can be enforced at all (security review §5). */
    if ((s->flags & CXEX_SEC_WRITE) && (s->flags & CXEX_SEC_EXEC))
        return CXEX_LOAD_WX;

    uint32_t va_lo = s->virt_addr & ~(CXEX_PAGE_SIZE - 1);
    *pages_out = (uint32_t)(((va_end - va_lo) + CXEX_PAGE_SIZE - 1) / CXEX_PAGE_SIZE);
    return CXEX_LOAD_OK;
}

int cxex_load(const uint8_t *file, size_t len,
              const struct cxex_load_ops *ops, uint32_t *entry_out) {
    if (!file || !ops || !ops->get_page || !ops->map_page)
        return CXEX_LOAD_BAD_FORMAT;

    /* A caller that forgot to set a limit would otherwise silently lose the
       user-half check, which is the one thing here that must never be optional. */
    if (ops->va_limit == 0) return CXEX_LOAD_BAD_FORMAT;

    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_LOAD_BAD_FORMAT;
    if (!(h.flags & CXEX_FLAG_EXECUTABLE))     return CXEX_LOAD_NOT_EXEC;

    /* ---- pass 1: validate everything, map nothing ----
       The loader used to validate each section as it mapped it, so a bad section
       half way through left the earlier ones already mapped into a live address
       space. Nothing is allocated or mapped until the whole image has passed. */
    uint32_t total_pages = 0;
    for (uint16_t i = 0; i < h.section_count; i++) {
        struct cxex_section s;
        if (cxex_get_section(file, len, &h, i, &s) != 0) return CXEX_LOAD_BAD_SECTION;
        if (s.mem_size == 0) continue;

        uint32_t pages = 0;
        int rc = check_section(&s, &h, len, ops->va_limit, &pages);
        if (rc != CXEX_LOAD_OK) return rc;

        total_pages += pages;
        if (total_pages > CXEX_LOAD_MAX_PAGES) return CXEX_LOAD_TOO_BIG;
    }

    /* The entry point has to be somewhere the image actually put code. */
    if (h.entry_point >= ops->va_limit) return CXEX_LOAD_BAD_RANGE;

    /* Paid for in full here, between the two passes: pass 1 has proved what the
       image needs and pass 2 has allocated nothing yet, so a refusal returns
       with no frames taken and nothing mapped. A load that fails LATER keeps the
       charge, which is correct - the pages it did take are still mapped, and the
       caller's teardown is what gives both back. */
    if (ops->charge_pages && ops->charge_pages(ops->ctx, total_pages) != 0)
        return CXEX_LOAD_QUOTA;

    /* ---- pass 2: allocate and map ---- */
    for (uint16_t i = 0; i < h.section_count; i++) {
        struct cxex_section s;
        if (cxex_get_section(file, len, &h, i, &s) != 0) return CXEX_LOAD_BAD_SECTION;
        if (s.mem_size == 0) continue;

        /* These sums are 32-bit and that is now safe: pass 1 proved
           virt_addr + mem_size <= va_limit, and va_limit is in the user half, so
           neither fb_hi nor va_end can reach the top of the address space, let
           alone wrap past it. Before pass 1 existed they could, and a wrapped
           va_end either skipped the loop entirely or ran it forever. */
        uint32_t fsz = (s.flags & CXEX_SEC_NOBITS) ? 0 : s.file_size;
        uint32_t prot   = prot_from_section(s.flags);
        uint32_t fb_lo  = s.virt_addr;             /* file-backed region, in VA */
        uint32_t fb_hi  = s.virt_addr + fsz;
        uint32_t va_lo  = s.virt_addr & ~(CXEX_PAGE_SIZE - 1);   /* page-align down */
        uint32_t va_end = s.virt_addr + s.mem_size;              /* exclusive */

        for (uint32_t pg = va_lo; pg < va_end; pg += CXEX_PAGE_SIZE) {
            uint32_t phys = 0;
            uint8_t *dst = (uint8_t *)ops->get_page(ops->ctx, &phys);
            if (!dst) return CXEX_LOAD_NOMEM;      /* page arrives zeroed (BSS-ready) */

            /* copy the slice of the file-backed region that falls in this page;
               everything else stays zero. */
            uint32_t pg_end = pg + CXEX_PAGE_SIZE;
            uint32_t cp_lo  = (pg     > fb_lo) ? pg     : fb_lo;   /* max */
            uint32_t cp_hi  = (pg_end < fb_hi) ? pg_end : fb_hi;   /* min */
            if (cp_lo < cp_hi) {
                uint32_t n       = cp_hi - cp_lo;
                uint32_t src_off = s.file_offset + (cp_lo - s.virt_addr);
                uint32_t dst_off = cp_lo - pg;
                for (uint32_t j = 0; j < n; j++) dst[dst_off + j] = file[src_off + j];
            }

            if (ops->map_page(ops->ctx, pg, phys, prot) != 0)
                return CXEX_LOAD_MAP_FAIL;
        }
    }

    if (entry_out) *entry_out = h.entry_point;
    return CXEX_LOAD_OK;
}

const char *cxex_load_strerror(int r) {
    switch (r) {
        case CXEX_LOAD_OK:          return "ok";
        case CXEX_LOAD_BAD_FORMAT:  return "bad CXEX header or arguments";
        case CXEX_LOAD_NOT_EXEC:    return "image is not executable";
        case CXEX_LOAD_BAD_SECTION: return "malformed section entry";
        case CXEX_LOAD_OOB:         return "section data outside image";
        case CXEX_LOAD_NOMEM:       return "out of memory (get_page failed)";
        case CXEX_LOAD_MAP_FAIL:    return "page mapping failed";
        case CXEX_LOAD_BAD_RANGE:   return "section leaves the user half of the address space";
        case CXEX_LOAD_WX:          return "section is both writable and executable";
        case CXEX_LOAD_UNSIGNED:    return "section data lies outside the signed range";
        case CXEX_LOAD_TOO_BIG:     return "image asks for more pages than are allowed";
        case CXEX_LOAD_QUOTA:       return "image does not fit the process memory quota";
        default:                    return "unknown error";
    }
}