// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_sweeps.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The syscall failure sweeps, made from ring 3 (roadmap Phase 1, step 6b).
 * Each call goes in through the system page's stub and the gate as a
 * process's does (usermode_run_calls), with the 1.6a hooks armed around
 * ring 3 only: what is tested is the syscall's own path, decoders and copies
 * included, not a kernel function called from a kernel test.
 */

#ifndef KTEST_SWEEPS_H
#define KTEST_SWEEPS_H

/* Every syscall in the table but the six that end the caller, block for
   input or touch the power state, called with every argument 0 and with
   every argument hostile (a kernel address; its low half a huge number and
   a negative handle), holding no grant: none answers E_INTERNAL or faults,
   a refusal reaches no allocation, a call that does allocate and is failed
   answers E_NO_MEMORY, and none leaves anything behind. */
int ktest_syscall_census(void);

/* Failing in turn every allocation of each syscall that makes a handle,
   a registration or a mapping: each run answers E_NO_MEMORY and leaves
   every live count as it was. */
int ktest_syscall_sweeps(void);

#endif
