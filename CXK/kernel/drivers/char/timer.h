/* /CXLite/kernel/drivers/timer.h */
/* Aurora Tejeda */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Set up the PIT to fire IRQ0 at TIMER_HZ and install the handler. */
void timer_init(void);

/* Milliseconds since timer_init (at 1000 Hz, 1 tick = 1 ms). */
uint32_t timer_ticks(void);

/* Busy-wait for the given number of milliseconds. */
void timer_sleep(uint32_t ms);

#endif