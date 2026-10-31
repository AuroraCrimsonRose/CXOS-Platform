// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_races.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The races (roadmap Phase 1, step 6c): each ordering that one CPU almost
 * never produces by itself, made to happen at a race point
 * (diag/testhook.h) and its outcome checked. The calls come from ring 3,
 * as a process makes them (usermode_run_calls).
 */

#ifndef KTEST_RACES_H
#define KTEST_RACES_H

/* A futex word unmapped after the wait faulted it in and before it reads
   it: E_BAD_ADDRESS, no waiter left queued, the key's reference on the
   memory object dropped (nothing left behind). */
int ktest_futex_unmapped(void);

/* A waiter whose deadline has come, woken before it looks again: it
   returns E_OK, and the wake counted it. */
int ktest_futex_deadline(void);

/* One memory object mapped in two processes at different addresses: a
   waiter in one is woken through the other's mapping, and not through a
   mapping of another object at the same offset. */
int ktest_futex_shared(void);

#endif
