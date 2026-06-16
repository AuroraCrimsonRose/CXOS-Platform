/* /CXLite/kernel/drivers/timer.c */
/* Aurora Tejeda */

#include <stdint.h>
#include "timer.h"
#include "idt.h"

#define PIT_BASE_FREQ 1193182u
#define TIMER_HZ      1000u            /* 1000 Hz -> ~1 ms per tick */

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static volatile uint32_t ticks = 0;

static void timer_callback(struct registers *r) {
    (void)r;
    ticks++;
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

    uint32_t target = ticks + ms;
    while (ticks < target) {
        __asm__ volatile ("hlt");
    }
}

void timer_init(void) {
    uint32_t divisor = PIT_BASE_FREQ / TIMER_HZ;   /* 1193182/1000 = 1193 */

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, timer_callback);
}