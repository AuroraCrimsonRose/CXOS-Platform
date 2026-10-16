// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/sync/atomic.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Atomic integers, over the compiler's __atomic builtins.
 *
 * The kernel runs on one CPU today, but kernel objects are written to be safe
 * on several from the start (docs/kernel/CX_KERNEL_PLAN.md section 2.1), so a
 * reference count is an atomic now rather than a plain int that SMP would
 * have to find and replace. Every operation here is sequentially consistent:
 * the counts these guard are not hot enough to justify weaker orderings, and
 * a wrong relaxed ordering is a bug no single-CPU test can show.
 *
 * Plain functions, not macros, so a type mismatch is a compile error.
 */
#ifndef CXK_ATOMIC_H
#define CXK_ATOMIC_H

#include <stdint.h>

typedef struct { volatile uint32_t v; } atomic_u32;

static inline uint32_t atomic_load(const atomic_u32 *a) {
    return __atomic_load_n(&a->v, __ATOMIC_SEQ_CST);
}

static inline void atomic_store(atomic_u32 *a, uint32_t x) {
    __atomic_store_n(&a->v, x, __ATOMIC_SEQ_CST);
}

/* Both return the value BEFORE the operation, as the builtins do. */
static inline uint32_t atomic_fetch_add(atomic_u32 *a, uint32_t x) {
    return __atomic_fetch_add(&a->v, x, __ATOMIC_SEQ_CST);
}

static inline uint32_t atomic_fetch_sub(atomic_u32 *a, uint32_t x) {
    return __atomic_fetch_sub(&a->v, x, __ATOMIC_SEQ_CST);
}

/* Replaces *a with `desired` if it holds `expected`. Returns 1 if it did. */
static inline int atomic_cas(atomic_u32 *a, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(&a->v, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#endif
