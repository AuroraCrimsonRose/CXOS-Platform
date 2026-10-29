// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/evtimer.h */
/* Aurora Tejeda / CATX Systems */
/*
 * What drivers/char/timer.c asks of the processor to keep time without a
 * fixed tick: a cycle counter it can make a clock of, and a timer of the
 * CPU's own that interrupts once after a count it is given. timer.c
 * calibrates both; nothing here knows a frequency. On x86 they are the TSC
 * and the local APIC's timer in one-shot mode (arch/x86/apic.c).
 */

#ifndef ARCH_EVTIMER_H
#define ARCH_EVTIMER_H

#include <stdint.h>
#include "arch/irq.h"

/* How far the cycle counter (arch_cycles64) can be trusted as a clock:
   0, there is none; 1, it counts at one rate while the CPU runs at one;
   2, at one rate whatever the CPU does (invariant: P-states, halts). */
int arch_cycles_constant(void);

/* Ready the timer, stopped, with `on_fire` called from its interrupt (before
   the EOI, as an IRQ handler is). -1 when the CPU has none to offer. */
int arch_evtimer_init(irq_handler_t on_fire);

/* Interrupt once after `count` of the timer's own steps; 0 stops it. A new
   count replaces the one running. */
void arch_evtimer_start(uint32_t count);

/* Steps left of the count running, 0 once it has fired or stopped. */
uint32_t arch_evtimer_left(void);

#endif
