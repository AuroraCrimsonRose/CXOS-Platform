/* /CXK/kernel/lib/format/cxex_load.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
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

int cxex_load(const uint8_t *file, size_t len,
              const struct cxex_load_ops *ops, uint32_t *entry_out) {
    if (!file || !ops || !ops->get_page || !ops->map_page)
        return CXEX_LOAD_BAD_FORMAT;

    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_LOAD_BAD_FORMAT;
    if (!(h.flags & CXEX_FLAG_EXECUTABLE))     return CXEX_LOAD_NOT_EXEC;

    for (uint16_t i = 0; i < h.section_count; i++) {
        struct cxex_section s;
        if (cxex_get_section(file, len, &h, i, &s) != 0) return CXEX_LOAD_BAD_SECTION;
        if (s.mem_size == 0) continue;

        /* file-backed byte count for this section: a NOBITS (BSS) section has
           none; otherwise file_size bytes are copied and the tail up to
           mem_size is left zero. */
        uint32_t fsz = (s.flags & CXEX_SEC_NOBITS) ? 0 : s.file_size;
        if (fsz && (uint64_t)s.file_offset + fsz > len) return CXEX_LOAD_OOB;

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
        default:                    return "unknown error";
    }
}