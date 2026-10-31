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

/* Failing each allocation in turn of pipe, memory object, job, process,
   held thread and mapping creation: each run refuses cleanly and leaves
   every live count as it was. */
int ktest_alloc_sweeps(void);

#endif
