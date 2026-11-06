// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/ktest_umem.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The self-tests' direct view of a test thread's user pages (1.9e4).
 *
 * A self-test sets up a call in its own user-half buffer from ring 0 and
 * reads the answer back the same way, while no other thread shares the
 * space. A syscall cannot assume that: a sibling on another CPU can unmap
 * the range between a check and a read, so every syscall goes through
 * user_copy_in/out (cpu/usermode.h). This header is the only other way a
 * user address becomes a pointer, and the build refuses it outside the
 * self-tests (tools/cmake/check_arch_boundary.cmake): ktest*.c, and
 * cpu/usermode.c, which includes it where its ring-3 tests begin, after
 * every syscall, so none of them can call it.
 */
#ifndef KTEST_UMEM_H
#define KTEST_UMEM_H

#include <stdint.h>

static inline void *ktest_umem(uint64_t uaddr) { return (void *)(uintptr_t)uaddr; }

#endif
