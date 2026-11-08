// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_panic.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The panic screen's parts a boot can check without panicking (roadmap Phase
 * 2 step 3a). The screen itself is checked by a real panic, screendumped and
 * decoded on the host (`cxk verify-crashlog --screendump`, PanicScreenTests).
 */
#ifndef KTEST_PANIC_H
#define KTEST_PANIC_H

/* 3 px per module fits, 2 does not, 8 is the most; 1024x768 shows the
   largest code and 640x480 none. */
int ktest_panic_fit(void);

/* The fixed record, encoded as this kernel was built, against its text and
   module fingerprints. */
int ktest_panic_qr(void);

/* The embedded table names real functions at the addresses of this link. */
int ktest_panic_ksym(void);

/* A walk from a known call chain finds it, by name, and ends at a wild
   frame without reading it. */
int ktest_panic_walk(void);

#endif
