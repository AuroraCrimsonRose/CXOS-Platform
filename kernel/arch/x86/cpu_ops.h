// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/cpu_ops.h */
/* Aurora Tejeda / CATX Systems */
/* arch/cpu_ops.h on x86, the same in both widths. The flags word is cut to
   32 bits: the only bit read back is IF, bit 9. */

#ifndef ARCH_X86_CPU_OPS_H
#define ARCH_X86_CPU_OPS_H

#include <stdint.h>

#define X86_EFLAGS_IF 0x200u

static inline uint32_t arch_flags_read(void) {
    uintptr_t f;
    __asm__ volatile ("pushf; pop %0" : "=r"(f) : : "memory");
    return (uint32_t)f;
}

static inline uint32_t arch_irq_save(void) {
    uintptr_t f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return (uint32_t)f;
}

static inline void arch_irq_restore(uint32_t f) {
    if (f & X86_EFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
}

static inline int arch_irqs_enabled(void) {
    return (arch_flags_read() & X86_EFLAGS_IF) != 0;
}

static inline void arch_irq_enable(void)  { __asm__ volatile ("sti" : : : "memory"); }
static inline void arch_irq_disable(void) { __asm__ volatile ("cli" : : : "memory"); }
static inline void arch_cpu_relax(void)   { __asm__ volatile ("pause"); }
static inline void arch_cpu_halt(void)    { __asm__ volatile ("hlt" : : : "memory"); }

/* sti takes effect after the NEXT instruction, so an interrupt pending at the
   sti is taken after the hlt has begun, and wakes it. Two separate calls would
   leave a window in which it is taken first and the hlt then waits for
   another. */
static inline void arch_cpu_idle(void) { __asm__ volatile ("sti; hlt" : : : "memory"); }
static inline void arch_cpu_idle_irqs_off(void) {
    __asm__ volatile ("sti; hlt; cli" : : : "memory");
}

__attribute__((noreturn)) static inline void arch_cpu_halt_forever(void) {
    __asm__ volatile ("cli" : : : "memory");
    for (;;) __asm__ volatile ("hlt");
}

static inline uint32_t arch_cycles32(void) {
    uint32_t lo;
    __asm__ volatile ("rdtsc" : "=a"(lo) : : "edx");
    return lo;
}

static inline uint64_t arch_cycles64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

#endif
