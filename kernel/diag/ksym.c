// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/ksym.c */
/* Aurora Tejeda / CATX Systems */
#include "diag/ksym.h"

const char *ksym_find(const uint32_t *offs, const uint32_t *name_offs, const char *names,
                      uint32_t count, uint32_t offset, uint32_t *off_out) {
    if (count == 0 || offset < offs[0]) return 0;
    /* The last entry at or before `offset`. */
    uint32_t lo = 0, hi = count;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (offs[mid] <= offset) lo = mid; else hi = mid;
    }
    if (off_out) *off_out = offset - offs[lo];
    return names + name_offs[lo];
}

#ifndef CXK_HOST_TEST
extern const char __text_start[], __text_end[];

const char *ksym_lookup(uintptr_t addr, uint32_t *off_out) {
    uintptr_t lo = (uintptr_t)__text_start, hi = (uintptr_t)__text_end;
    if (addr < lo || addr >= hi) return 0;
    return ksym_find(ksym_offs, ksym_name_offs, ksym_names, ksym_count,
                     (uint32_t)(addr - lo), off_out);
}
#endif
