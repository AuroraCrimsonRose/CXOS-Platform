// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_hooks.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The self-tests of the test hooks (diag/testhook.h), and the first sweeps
 * that use them (roadmap Phase 1, step 6a). Kept out of ktest.c because the
 * hooks are the tool every later failure and race test is built with: each
 * is shown to work here before anything relies on it.
 */

#ifndef KTEST_HOOKS_H
#define KTEST_HOOKS_H

/* Each allocation site fails at the Nth call, of the arming thread only,
   and of the sites armed only. */
int ktest_fault_nth(void);

/* Live counts are stable at rest, and see a block, a page, a cache object,
   a process, a mapping and a job's charge each come and go. */
int ktest_live_counts(void);

/* A race point calls at its Nth arrival, once; parks only the thread it
   was armed for, until released. */
int ktest_race_points(void);

/* Chaos: its yields are the documented sequence's, from the seed alone
   (the same seed the same, another other); at 1000 every arrival yields
   and another thread runs; a point not chosen never yields; an armed race
   point still fires once; bad arguments refused; the boot's chaos back. */
int ktest_chaos(void);

/* Failing each allocation in turn of pipe, memory object, job, process,
   held thread and mapping creation: each run refuses cleanly and leaves
   every live count as it was. */
int ktest_alloc_sweeps(void);

/* The lock order (1.9a): locks taken in order are listed and their pair
   recorded; out of order, two of one rank, an unranked lock, a release not
   held and a thread switch holding one are each caught; the heap's large
   path (alloc, free, the count) takes the heap lock. */
int ktest_lock_order(void);
int ktest_sched_lock(void);

#endif
