// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/cpu_ops.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The few things generic code asks of the processor often enough that a call
 * would cost more than the instruction: the interrupt flag, waiting for an
 * interrupt, and a cycle count. Each architecture supplies them inline in
 * kernel/arch/<arch>/cpu_ops.h; nothing outside kernel/arch/ writes assembly.
 *
 * Built for the host tests (CXK_HOST_TEST) they do nothing, so the logic
 * above them is testable.
 */

#ifndef ARCH_CPU_OPS_H
#define ARCH_CPU_OPS_H

#include <stdint.h>

/*
 * arch_irq_save()          interrupts off; returns the state to give back
 * arch_irq_restore(f)      interrupts back on only if they were on at the save
 * arch_irqs_enabled()      1 if interrupts are on on this CPU
 * arch_irq_enable()        interrupts on
 * arch_irq_disable()       interrupts off
 * arch_cpu_relax()         a hint inside a spin loop
 * arch_cpu_halt()          wait for the next interrupt, interrupt state as is
 *                          (with interrupts off that is a wait for an NMI)
 * arch_cpu_idle()          interrupts on and wait for one, with no gap between
 *                          the two in which an interrupt could be missed
 * arch_cpu_idle_irqs_off() the same, then interrupts off again
 * arch_cpu_halt_forever()  interrupts off and stop; never returns
 * arch_cycles32()          low 32 bits of a counter that advances with the
 *                          clock (wraps in seconds; compare by difference)
 */

#if defined(CXK_HOST_TEST)
static inline uint32_t arch_irq_save(void)          { return 0; }
static inline void     arch_irq_restore(uint32_t f) { (void)f; }
static inline int      arch_irqs_enabled(void)      { return 1; }
static inline void     arch_irq_enable(void)        { }
static inline void     arch_irq_disable(void)       { }
static inline void     arch_cpu_relax(void)         { }
static inline void     arch_cpu_halt(void)          { }
static inline void     arch_cpu_idle(void)          { }
static inline void     arch_cpu_idle_irqs_off(void) { }
static inline uint32_t arch_cycles32(void)          { return 0; }
__attribute__((noreturn)) static inline void arch_cpu_halt_forever(void) { for (;;) { } }
#elif defined(__i386__) || defined(__x86_64__)
#include "arch/x86/cpu_ops.h"
#else
#error "no cpu_ops for this architecture"
#endif

#endif
