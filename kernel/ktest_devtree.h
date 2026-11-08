// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_devtree.h */
/* Aurora Tejeda / CATX Systems */
/* The device tree's self-tests (Phase 2.1): 1 = pass. */
#ifndef KTEST_DEVTREE_H
#define KTEST_DEVTREE_H

int ktest_devtree_specific(void);
int ktest_devtree_fallback(void);
int ktest_devtree_order(void);
int ktest_devtree_gone(void);
int ktest_devtree_boot(void);

#endif
