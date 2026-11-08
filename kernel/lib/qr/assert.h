// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/qr/assert.h */
/* Aurora Tejeda / CATX Systems */
/* For vendor/qrcodegen/qrcodegen.c alone, which is kept unmodified and asks
   for the hosted <assert.h> (the -I for this directory is on that one file).
   An assertion that fails there is a broken encoder. On the panic path that
   lands in the panic-inside-a-panic branch, which ends without the screen. */
#ifndef QR_ASSERT_H
#define QR_ASSERT_H

__attribute__((noreturn)) void kpanic(const char *why);

#define assert(x) ((x) ? (void)0 : kpanic("qrcodegen: an assertion failed"))

#endif
