// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/i8042.c */
/* Aurora Tejeda / CATX Systems */

#include <stdint.h>
#include "i8042.h"
#include "drivers/iowin.h"

/* data at 0x60; status (read) and command (write) share 0x64 */
static struct iowin data = IOWIN_PORTS(0x60, 1);
static struct iowin ctl  = IOWIN_PORTS(0x64, 1);

/* A missing controller reads all ones, which looks permanently busy, so every
   wait gives up rather than hang the machine. */
#define I8042_SPINS 100000

uint8_t i8042_status(void) { return iowin_read8(&ctl, 0); }
uint8_t i8042_data(void)   { return iowin_read8(&data, 0); }

static void wait_input(void) {          /* until we may write */
    for (int i = 0; i < I8042_SPINS; i++)
        if (!(i8042_status() & I8042_STAT_INPUT_FULL)) return;
}

static void wait_output(void) {         /* until there is a byte to read */
    for (int i = 0; i < I8042_SPINS; i++)
        if (i8042_status() & I8042_STAT_OUTPUT_FULL) return;
}

void i8042_command(uint8_t cmd) { wait_input(); iowin_write8(&ctl, 0, cmd); }
void i8042_write(uint8_t v)     { wait_input(); iowin_write8(&data, 0, v); }

uint8_t i8042_read(void) {
    wait_output();
    return i8042_data();
}

void i8042_pulse_reset(void) {
    /* Empty the output buffer and let the input buffer drain, so the command
       is not taken as the tail of something else. Bounded: this used to loop
       while the input buffer was full, and with no 8042 that is forever, so
       the triple-fault fallback after it was never reached. */
    for (int i = 0; i < I8042_SPINS; i++) {
        uint8_t status = i8042_status();
        if (status & I8042_STAT_OUTPUT_FULL) (void)i8042_data();
        if (!(status & I8042_STAT_INPUT_FULL)) break;
    }
    iowin_write8(&ctl, 0, I8042_CMD_PULSE_RESET);
}
