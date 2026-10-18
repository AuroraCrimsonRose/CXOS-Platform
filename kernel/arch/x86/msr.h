// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/msr.h */
/* Aurora Tejeda / CATX Systems */
/* Model-specific registers. Needed to find and enable the local APIC: its base
   address lives in IA32_APIC_BASE (MSR 0x1B), not in config space or ACPI. */

#ifndef ARCH_X86_MSR_H
#define ARCH_X86_MSR_H

#include <stdint.h>

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                                  "d"((uint32_t)(value >> 32)));
}

#endif
