// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/xdata64.c */
/* Aurora Tejeda / CATX Systems */
/*
 * TRANSITIONAL (docs/planning/TRANSITIONAL_CODE.md): lib/format/xdata.h with
 * no reader behind it. The 32-bit kernel links os/std/xdata.xfxn, compiled by
 * `cxk compile --object`; X compiles to x86-32 only until roadmap step 6, so
 * there is nothing here to link. A C port would be a third reader, which is
 * what linking the X one avoided (xdata.h), so this refuses every document
 * instead: xd_check fails at offset 0 with a reason that says why, kconfig
 * refuses kernel.xkco whole and boots on its built-in defaults (kconfig.c),
 * and the self-test checks exactly that (ktest.c, test_kconfig). Goes when X
 * compiles for x86-64 and xdata.xfxn is linked here as on x86-32.
 *
 * Only xd_check is reached: every caller stops at its refusal. The rest
 * refuse too, so a caller that did not stop is refused, not misled.
 */

#include "lib/format/xdata.h"

int32_t  xd_err;
uint32_t xd_err_at;

int32_t xd_check(const char *s, uint32_t len) {
    (void)s;
    (void)len;
    xd_err = XD_E_SYNTAX;
    xd_err_at = 0;
    return xd_err;
}

const char *xd_strerror(int32_t code) {
    (void)code;
    return "no X Data reader on x86-64 until X compiles for it (roadmap step 6)";
}

uint32_t xd_line(const char *s, uint32_t at) {
    (void)s;
    (void)at;
    return 1;
}

void xd_root(const char *s, uint32_t len, struct xd_val *out) {
    (void)s;
    (void)len;
    out->kind = 0;   /* no kind: not a record, so nothing is read from it */
    out->at = out->len = out->tag_at = out->tag_len = 0;
}

int32_t xd_get(const char *s, struct xd_val *rec, const char *key, struct xd_val *out) {
    (void)s; (void)rec; (void)key; (void)out;
    return XD_E_TYPE;
}

int32_t xd_entry_at(const char *s, struct xd_val *rec, uint32_t index,
                    struct xd_val *key, struct xd_val *val) {
    (void)s; (void)rec; (void)index; (void)key; (void)val;
    return XD_E_TYPE;
}

int32_t xd_u32(const char *s, struct xd_val *v, uint32_t *out) {
    (void)s; (void)v; (void)out;
    return XD_E_TYPE;
}

int32_t xd_sym_is(const char *s, struct xd_val *v, const char *name) {
    (void)s; (void)v; (void)name;
    return 0;
}
