// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/smp.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The other CPUs (1.9d): started and parked, running nothing yet.
 *
 * arch_smp_start starts every CPU the firmware lists as usable, one at a
 * time, by the ID it lists it under, and each sets up its own tables,
 * per-CPU data and interrupt controller, then parks: halted, interrupts on,
 * woken only by smp_call's IPI. It joins cpu_count() once parked
 * (cpu/percpu.h). A CPU that never answers is put back to sleep and left
 * out. Running threads on them is 1.9e. Design: docs/kernel/CX_SMP.md.
 */
#ifndef ARCH_SMP_H
#define ARCH_SMP_H

#include <stdint.h>

/* After the interrupt controllers and the clock (kmain), on the boot CPU,
   interrupts on. Returns the CPUs running, the boot CPU included. */
unsigned arch_smp_start(void);

/* Run fn(arg) on parked CPU `index` and wait for it to finish, up to
   `timeout_ns` (timer_now_ns). It runs in the park loop with interrupts
   off, so it may not block, take a lock or log; it may enable interrupts
   for a moment, as the timer's self-test does. 0 when it ran, -1 when the
   CPU is not a parked AP or did not finish in time (it may still run it). */
int smp_call(unsigned index, void (*fn)(void *), void *arg, uint64_t timeout_ns);

#endif
