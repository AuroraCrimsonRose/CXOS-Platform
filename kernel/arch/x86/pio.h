// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/pio.h */
/* Aurora Tejeda / CATX Systems */
/* arch/pio.h on x86: the in and out instructions, the same in both widths. */

#ifndef ARCH_X86_PIO_H
#define ARCH_X86_PIO_H

#include <stdint.h>

#define ARCH_HAS_PIO 1

static inline uint8_t arch_pio_in8(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint16_t arch_pio_in16(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t arch_pio_in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void arch_pio_out8(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void arch_pio_out16(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline void arch_pio_out32(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

#endif
