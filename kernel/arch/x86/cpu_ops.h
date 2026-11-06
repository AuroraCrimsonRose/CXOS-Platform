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

/* This CPU's own data (cpu/percpu.h), through the GS base: its first word is
   its address. Volatile, so a read is never carried across a switch, after
   which the thread may be on another CPU. */
static inline void *arch_cpu_self(void) {
    void *c;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* One 32-bit field of it, in one instruction: no switch can land between
   finding the CPU and reading the field. */
static inline uint32_t arch_cpu_local_u32(uint32_t off) {
    uint32_t v;
    __asm__ volatile ("movl %%gs:(%1), %0" : "=r"(v) : "r"((uintptr_t)off));
    return v;
}

/* A pointer-sized one, the same way (1.9e2: the running thread's record). */
static inline void *arch_cpu_local_ptr(uint32_t off) {
    void *v;
    __asm__ volatile ("mov %%gs:(%1), %0" : "=r"(v) : "r"((uintptr_t)off));
    return v;
}

#endif
