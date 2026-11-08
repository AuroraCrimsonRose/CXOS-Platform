// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/qr/stdlib.h */
/* Aurora Tejeda / CATX Systems */
/* For vendor/qrcodegen/qrcodegen.c alone (see assert.h beside this): the
   few things it wants that the kernel does not have. */
#ifndef QR_STDLIB_H
#define QR_STDLIB_H

static inline int abs(int v) { return v < 0 ? -v : v; }
static inline long labs(long v) { return v < 0 ? -v : v; }

/* <string.h>'s, which the kernel's own string.h has no other use for. */
static inline char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

#endif
