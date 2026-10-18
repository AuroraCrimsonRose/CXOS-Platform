// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/pit.c */
/* Aurora Tejeda / CATX Systems */

#include <stdint.h>
#include "pit.h"
#include "drivers/iowin.h"

/* channels 0..2 at +0..+2, the mode/command register at +3 */
static struct iowin pit = IOWIN_PORTS(0x40, 4);
#define PIT_CHANNEL0  0
#define PIT_CHANNEL2  2
#define PIT_COMMAND   3

/* A 16-bit counter: 0 means 65536, so anything slower than ~18.2 Hz cannot be
   expressed and is clamped to that. */
static uint16_t divisor_for(uint32_t hz) {
    if (hz == 0) hz = 1;
    uint32_t d = PIT_BASE_FREQ / hz;
    if (d == 0) d = 1;
    if (d > 0xFFFF) d = 0;
    return (uint16_t)d;
}

void pit_start_periodic(uint32_t hz, irq_handler_t on_tick) {
    uint16_t divisor = divisor_for(hz);

    /* Channel 0, low then high byte, mode 2 (rate generator): one short pulse
       per period. Mode 3, the square wave, was used until the tick-rate
       self-test measured it against the ACPI PM timer: through the I/O APIC
       QEMU delivered two interrupts per period, so every "millisecond" was
       half of one. The 8259 counted it right, which is why nothing noticed.
       Linux programs the periodic PIT in mode 2 too (0x34). */
    iowin_write8(&pit, PIT_COMMAND, 0x34);
    iowin_write8(&pit, PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    iowin_write8(&pit, PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    irq_install_handler(0, on_tick);
}

void pit_channel2_square(uint32_t hz) {
    uint16_t divisor = divisor_for(hz);

    iowin_write8(&pit, PIT_COMMAND, 0xB6);      /* channel 2, lo/hi, mode 3 */
    iowin_write8(&pit, PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    iowin_write8(&pit, PIT_CHANNEL2, (uint8_t)(divisor >> 8));
}
