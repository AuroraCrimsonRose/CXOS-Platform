// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/timer.c */
/* Aurora Tejeda */

#include <stdint.h>
#include "sched.h"
#include "timer.h"
#include "keyboard.h"
#include "idt.h"
#include "io.h"

#define PIT_BASE_FREQ 1193182u
#define TIMER_HZ      1000u            /* 1000 Hz -> ~1 ms per tick */

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

static volatile uint32_t ticks = 0;

static void timer_callback(struct registers *r) {
    (void)r;
    ticks++;
    sched_tick();          /* preemptive scheduler (no-op unless enabled) */
    sched_wake_sleepers(); /* SYS_SLEEP: put expired sleepers back on the queue */
    keyboard_tick();       /* wake a USB-HID reader; raises no bus traffic here */
}

uint32_t timer_ticks(void) {
    return ticks;
}

void timer_sleep(uint32_t ms) {
    /* If interrupts are disabled, `ticks` can never advance (the PIT IRQ can't
       fire), so the normal wait would hang forever. Detect that and fall back
       to a bounded busy-delay instead - prevents a deadlock if timer_sleep is
       ever called during early boot init (before sti) or inside a cli section. */
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0" : "=r"(flags));
    if (!(flags & 0x200)) {              /* IF (bit 9) clear = interrupts off */
        volatile uint32_t x = 0;
        /* rough busy-delay: ~ a few hundred thousand iterations per ms */
        for (uint32_t i = 0; i < ms * 200000u; i++) x += i;
        return;
    }

    /* Elapsed-since rather than a `ticks < target` deadline: the sum wraps
       every 49.7 days, and a wrapped target compares small, so the wait would
       end immediately. An unsigned difference does not care where the wrap
       falls. See the note on timer_tick_after in timer.h. */
    uint32_t start = ticks;
    while ((uint32_t)(ticks - start) < ms) {
        __asm__ volatile ("hlt");
    }
}

/* ---- bounded-wait helpers (see timer.h) ---- */

/* spin budget per millisecond when interrupts are off (rough, deliberately
   generous so a slow device isn't cut off early on real hardware). */
#define TIMEOUT_SPINS_PER_MS  200000u

void timer_timeout_start(struct timeout *to, uint32_t ms) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0" : "=r"(flags));
    if (flags & 0x200) {                 /* IF set = interrupts on: use ticks */
        to->use_timer  = 1;
        to->deadline   = ticks + ms + 1; /* +1 so sub-ms budgets get a full tick */
        to->spins_left = 0;
    } else {                             /* interrupts off: bounded spin */
        to->use_timer  = 0;
        to->deadline   = 0;
        to->spins_left = ms * TIMEOUT_SPINS_PER_MS;
        if (to->spins_left == 0) to->spins_left = TIMEOUT_SPINS_PER_MS;
    }
}

int timer_timeout_expired(struct timeout *to) {
    if (to->use_timer) {
        /* Wrap-safe: a plain `ticks >= to->deadline` reports expiry the
           instant the deadline sum wraps, which would fail a disk transfer
           with DISK_ERR_TIMEOUT before the hardware was even asked. */
        return timer_tick_after(ticks, to->deadline) ? 1 : 0;
    }
    /* spin mode: each call burns one unit of budget */
    if (to->spins_left == 0) return 1;
    to->spins_left--;
    return 0;
}

void timer_init(void) {
    uint32_t divisor = PIT_BASE_FREQ / TIMER_HZ;   /* 1193182/1000 = 1193 */

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, timer_callback);
}

/* ---- TSC: sub-millisecond timing ----
 * The PIT runs at 1000 Hz, so timer_ticks() cannot express anything shorter
 * than a millisecond - which is why every ping measured 0 ms even when it was
 * genuinely working. The TSC counts CPU cycles and gives us microseconds.
 *
 * Everything here is deliberately 32-bit: a freestanding kernel has no libgcc,
 * so 64-bit division would fail to link (__udivdi3). The low 32 bits of the TSC
 * wrap about every 1.4 s at 3 GHz, which is comfortably longer than any interval
 * we measure, and unsigned subtraction wraps correctly.
 */
static uint32_t tsc_per_us = 0;      /* cycles per microsecond; 0 = not calibrated */

static inline uint32_t rdtsc32(void) {
    uint32_t lo;
    __asm__ volatile ("rdtsc" : "=a"(lo) : : "edx");
    return lo;
}

uint32_t timer_tsc32(void) { return rdtsc32(); }

/* Calibrate against the PIT. Called once, after the timer is running. */
void timer_calibrate_tsc(void) {
    uint32_t t0 = timer_ticks();
    while (timer_ticks() == t0) { }          /* align to a tick edge */

    uint32_t c0    = rdtsc32();
    uint32_t start = timer_ticks();
    while ((timer_ticks() - start) < 50) { } /* 50 ms window */
    uint32_t cycles = rdtsc32() - c0;

    tsc_per_us = cycles / 50000u;            /* 50 ms = 50000 us */
    if (tsc_per_us == 0) tsc_per_us = 1;     /* never divide by zero */
}

/* Microseconds elapsed since a timer_tsc32() sample. */
uint32_t timer_us_since(uint32_t start) {
    if (!tsc_per_us) return 0;
    return (rdtsc32() - start) / tsc_per_us;
}

uint32_t timer_tsc_mhz(void) { return tsc_per_us; }

/* ---- testing only: see timer.h ---- */
void timer_ticks_set_for_test(uint32_t t) {
    ticks = t;
}
