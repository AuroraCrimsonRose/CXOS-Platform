// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/ktest_platform.h */
/* Aurora Tejeda / CATX Systems */
/* Boot self-tests that drive a platform's own devices (ktest.c reports them).
   Each returns 1 for a pass and 0 for a fail, logging why. */

#ifndef PLATFORM_KTEST_PLATFORM_H
#define PLATFORM_KTEST_PLATFORM_H

/* The built-in keyboard and mouse reach the kernel's input buffers. */
int ktest_platform_input(void);

#endif
