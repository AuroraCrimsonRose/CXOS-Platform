// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/ktest_arch.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Boot self-tests that have to look at the processor's own state, which no
 * file outside kernel/arch/ may do. ktest.c runs and reports them like its
 * own. Each returns 1 for a pass.
 */

#ifndef ARCH_KTEST_ARCH_H
#define ARCH_KTEST_ARCH_H

/* A double fault runs on a stack of its own, so a kernel stack overflow is
   reported rather than becoming a reset. */
int ktest_arch_double_fault_stack(void);

/* The CPU feature table was read from this CPU, and the interrupt code
   obeyed it. */
int ktest_arch_cpu_features(void);

/* A syscall gives ring 3 its data segment registers back, not the kernel's
   (which the return to ring 3 would turn into NULL). */
int ktest_arch_user_segments_kept(void);

/* The architecture's own further tests, which ktest_run runs and reports
   after its own: *count of them, each named. None on x86-32, whose arch tests
   are the three above; x86-64's bring-up tests (steps 5b to 5e). */
struct ktest_arch_case {
    const char *name;
    int (*fn)(void);
};
const struct ktest_arch_case *ktest_arch_cases(unsigned *count);

#endif
