// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_stress.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The SMP stress suite (roadmap 1.9f, docs/kernel/CX_SMP.md §4a): the whole
 * self-test boot on 8 CPUs under QEMU's multi-threaded TCG (`cxk verify-boot
 * --stress`), Hyper-V and the rig, with sched_lock's contention counted
 * (sync/lockstat.h) and printed after the suite as a profile in the hardware
 * report's form. Test-hook builds only; without the hooks each test fails.
 */

#ifndef KTEST_STRESS_H
#define KTEST_STRESS_H

/* The count itself: the boot CPU holds sched_lock while AP 1 takes it; the
   AP's take is counted contended, with spins and its wait, and the boot
   CPU's longest hold covers the hold it timed. Passes with one CPU. */
int ktest_lockstat_count(void);

/* A token round a thread on every CPU, 200 laps, each pass a cross-CPU wake;
   then every thread yields 2000 times at once. Every pass made, no wait
   reaching its deadline (a wake lost), nothing left behind. Passes with one
   CPU. Its counts are the profile's `stress` lines. */
int ktest_smp_stress(void);

/* The profile, on serial, after the suite's last test. */
void ktest_profile_report(void);

#endif
