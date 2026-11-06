// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/smp.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The other CPUs (1.9d): started, each with its own tables; (1.9e1) the
 * interrupts between CPUs and the TLB shootdown; and (1.9e2) the idle wait
 * and the IPI that hand a CPU a thread.
 *
 * arch_smp_start starts every CPU the firmware lists as usable, one at a
 * time, by the ID it lists it under, and each sets up its own tables,
 * per-CPU data and interrupt controller, parks until it is in cpu_count()
 * (cpu/percpu.h), then runs its idle thread (cpu/sched.c). A CPU that never
 * answers is put back to sleep and left out. Design: docs/kernel/CX_SMP.md.
 */
#ifndef ARCH_SMP_H
#define ARCH_SMP_H

#include <stdint.h>

/* After the interrupt controllers and the clock (kmain), on the boot CPU,
   interrupts on. Returns the CPUs running, the boot CPU included. */
unsigned arch_smp_start(void);

/* Run fn(arg) on AP `index` and wait for it to finish, up to `timeout_ns`
   (timer_now_ns). It runs in that CPU's idle thread (1.9e2: once nothing
   else runs there) with interrupts off, so it may not block, take a lock
   or log; it may enable interrupts for a moment, as the timer's self-test
   does. 0 when it ran, -1 when the CPU is not a running AP or did not
   finish in time (it may still run it).
   smp_call_start leaves the call and returns (0, or -1 refused);
   smp_call_wait waits for the one left (0, or -1 not in time). */
int smp_call(unsigned index, void (*fn)(void *), void *arg, uint64_t timeout_ns);
int smp_call_start(unsigned index, void (*fn)(void *), void *arg);
int smp_call_wait(unsigned index, uint64_t timeout_ns);

/* ---- the TSC's agreement (1.9e3) ----
 *
 * Every AP's TSC is measured against the boot CPU's as it parks, and set
 * right if it is out by more than the measure's own error and a margin
 * (XNU's, 4096 cycles), so the clock (timer_now_ns) reads the same on every
 * CPU. `before` is the AP's TSC less the boot CPU's, in cycles, as first
 * measured; `after` once set right (`before` when it was not); `err` the
 * measure's half-width.
 * smp_tsc_at_start is what bring-up found for AP `index` (0 for the boot
 * CPU or one past the last). smp_tsc_agree meets a running AP again, as
 * smp_call does, having first added `skew` cycles to its TSC (0: none): 0
 * agreed, 1 still out after setting it right, -1 no meeting. */
struct smp_tsc { int64_t before, after; uint64_t err; uint32_t adjusted; };
const struct smp_tsc *smp_tsc_at_start(unsigned index);
int smp_tsc_agree(unsigned index, int64_t skew, struct smp_tsc *out);

/* ---- TLB shootdown (1.9e1) ----
 *
 * After a mapping that was present is changed or removed, no CPU may go on
 * using the old one: smp_tlb_shootdown makes every other CPU that could
 * hold it drop it, and returns once each has. `root` is the space changed,
 * or SMP_TLB_KERNEL for the kernel half, which every space shares; `pages`
 * from `va`, everything past SMP_TLB_FULL_AT. The caller has flushed its
 * own TLB. A CPU is asked only if its CR3 holds `root` (any, for the
 * kernel half); one that is idle is marked stale and flushes all of it as
 * it wakes, never interrupted for it.
 *
 * Callable anywhere, interrupts on or off, holding spinlocks: a CPU
 * spinning on a lock serves requests while it spins (sync/spinlock.h), as
 * the sender does while it waits, so two shootdowns aimed at each other,
 * or one aimed at a CPU spinning on a lock its sender holds, both finish.
 * Returns 0, or the CPUs (a bit per index) that did not answer within
 * `timeout_ns`. The paging layer panics on any (arch/paging.h). */
#define SMP_TLB_KERNEL   (~0ull)
#define SMP_TLB_FULL_AT  32u
#define SMP_TLB_TIMEOUT_NS 10000000000ull   /* 10 s: a syscall's body runs with interrupts off */
uint32_t smp_tlb_shootdown(uint64_t root, uintptr_t va, uint32_t pages, uint64_t timeout_ns);

/* Serve whatever shootdowns are waiting for this CPU. Interrupts off; takes
   no lock. The IPI's handler calls it, and so does every spin. */
void smp_serve(void);

/* ---- the scheduler on every CPU (1.9e2) ----
 *
 * smp_kick interrupts CPU `index` (not this one) with the IPI vector, which
 * ends its halt or brings it to a preemption point (cpu/sched.c resched).
 * smp_idle_wait is an AP's idle thread's halt: a call left for it
 * (smp_call) is run, or else the CPU is marked idle for the shootdown and
 * halts until an interrupt; either way it returns, interrupts off as it
 * was entered, having flushed whatever was skipped while it was idle. */
void smp_kick(unsigned index);
void smp_idle_wait(void);

#endif
