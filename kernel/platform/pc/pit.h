// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/pit.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The 8253/8254 programmable interval timer: three counters fed by a
 * 1.193182 MHz clock. Channel 0 drives IRQ 0, the kernel's tick; channel 2
 * drives the speaker. This file is the only one that programs it.
 */

#ifndef PLATFORM_PC_PIT_H
#define PLATFORM_PC_PIT_H

#include <stdint.h>
#include "arch/irq.h"

#define PIT_BASE_FREQ 1193182u

/* Interrupt `hz` times a second on IRQ 0, calling on_tick each time. */
void pit_start_periodic(uint32_t hz, irq_handler_t on_tick);

/* A square wave of `hz` on channel 2, whose output reaches the speaker once
   speaker.c opens the gate. */
void pit_channel2_square(uint32_t hz);

#endif
