// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/pit.c */
/* Aurora Tejeda / CATX Systems */

#include <stdint.h>
#include "pit.h"
#include "drivers/iowin.h"
#include "arch/irq.h"
#include "arch/cpu_ops.h"

/* channels 0..2 at +0..+2, the mode/command register at +3 */
static struct iowin pit = IOWIN_PORTS(0x40, 4);
#define PIT_CHANNEL0  0
#define PIT_CHANNEL2  2
#define PIT_COMMAND   3

/* port B: bit 0 is channel 2's gate, bit 1 lets its output reach the speaker */
static struct iowin port_b = IOWIN_PORTS(0x61, 1);

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

/* ---- the cycle counter, against channel 2's count ----
 * Polled, never counted in interrupts: Hyper-V delivers the tick at its
 * host's pace (1.8c: about 64 a second for the 1000 asked), so a rate taken
 * over 50 tick interrupts came out 15.7 times too high. Channel 2 in mode 0
 * counts down from 0xFFFF on its own crystal; each sample latches the count
 * and reads the cycle counter with interrupts off, so one landing between
 * them does not count. A window is kept only when the next agrees with it
 * within 1%: an SMI or a host stall inside one stretches it (Haiku's
 * calculate_cpu_conversion_factor, arch_timer.cpp, polls channel 2 and
 * retries the same way; Serenity's TSC::calibrate reads its source's raw
 * counter for the same reason). */
#define CAL_WINDOW    59659u        /* 50 ms of PIT_BASE_FREQ */
/* A live count moves every 0.84 us, and a sample (three port accesses) takes
   longer than that, so 1000 samples in a row on one value is a count that
   stopped. Not a total: under Hyper-V each access exits to the host, and a
   cap of 5 million samples took minutes to give up on a shut gate (1.8c). */
#define CAL_STILL     1000u
#define CAL_POLLS     1000000u      /* the backstop: a window is ~10^4 */
#define CAL_TRIES     5

static uint16_t ch2_sample(uint64_t *cycles) {
    uint32_t f = arch_irq_save();
    iowin_write8(&pit, PIT_COMMAND, 0x80);      /* channel 2, latch the count */
    uint8_t lo = iowin_read8(&pit, PIT_CHANNEL2);
    uint8_t hi = iowin_read8(&pit, PIT_CHANNEL2);
    *cycles = arch_cycles64();
    arch_irq_restore(f);
    return (uint16_t)(lo | (uint16_t)hi << 8);
}

/* One window: cycles a second, or 0 if the count did not move. */
static uint64_t ch2_window(void) {
    iowin_write8(&pit, PIT_COMMAND, 0xB0);      /* channel 2, lo/hi, mode 0, binary */
    iowin_write8(&pit, PIT_CHANNEL2, 0xFF);
    iowin_write8(&pit, PIT_CHANNEL2, 0xFF);

    uint64_t c0, c1;
    uint16_t a = ch2_sample(&c0), start, now, last;
    uint32_t n = 0, still = 0;
    do {                                        /* from an edge of the count */
        start = ch2_sample(&c0);
        if (++n > CAL_POLLS || ++still > CAL_STILL) return 0;
    } while (start == a);
    uint16_t gone;
    last = start; still = 0;
    do {
        now  = ch2_sample(&c1);
        gone = (uint16_t)(start - now);          /* mode 0 wraps past 0; mod 2^16 */
        if (now != last) { last = now; still = 0; }
        if (++n > CAL_POLLS || ++still > CAL_STILL) return 0;
    } while (gone < CAL_WINDOW);
    return (c1 - c0) * PIT_BASE_FREQ / gone;
}

uint64_t pit_cycles_hz(void) {
    uint8_t b = iowin_read8(&port_b, 0);
    iowin_write8(&port_b, 0, (uint8_t)((b & ~2u) | 1u));   /* gate open, speaker off */

    uint64_t hz = 0, prev = ch2_window();
    for (int i = 0; i < CAL_TRIES && prev; i++) {
        uint64_t next = ch2_window();
        if (!next) break;
        uint64_t d = next > prev ? next - prev : prev - next;
        if (d <= prev / 100) { hz = next < prev ? next : prev; break; }
        prev = next;
    }

    iowin_write8(&port_b, 0, b);
    return hz;
}
