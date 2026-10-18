// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/pio.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Port I/O, for drivers/iowin.c only. Every other piece of generic code reaches
 * a port through an I/O window (drivers/iowin.h).
 *
 * ARCH_HAS_PIO says whether the architecture has a separate port space at all;
 * x86 does, ARM does not. The host tests supply their own arch_pio_* that
 * record each access instead of executing it.
 */

#ifndef ARCH_PIO_H
#define ARCH_PIO_H

#include <stdint.h>

#if defined(CXK_HOST_TEST)
#define ARCH_HAS_PIO 1
uint8_t  arch_pio_in8 (uint16_t port);
uint16_t arch_pio_in16(uint16_t port);
uint32_t arch_pio_in32(uint16_t port);
void     arch_pio_out8 (uint16_t port, uint8_t  v);
void     arch_pio_out16(uint16_t port, uint16_t v);
void     arch_pio_out32(uint16_t port, uint32_t v);
#elif defined(__i386__) || defined(__x86_64__)
#include "arch/x86/pio.h"
#else
#define ARCH_HAS_PIO 0
#endif

#endif
