// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#include <stdint.h>

#include "speaker.h"
#include "drivers/iowin.h"
#include "timer.h"
#include "pit.h"

/* PIT channel 2 drives the speaker; port 0x61 bits 0-1 gate it on */
static struct iowin port_b = IOWIN_PORTS(0x61, 1);

static uint8_t speaker_enabled = 0;

void speaker_init(void)
{
    speaker_enabled = 1;
}

void speaker_on(uint32_t freq)
{
    if (!speaker_enabled) return;
    if (freq < 20) freq = 20;

    pit_channel2_square(freq);

    uint8_t tmp = iowin_read8(&port_b, 0);

    if ((tmp & 3) != 3)
        iowin_write8(&port_b, 0, tmp | 3);
}

void speaker_off(void)
{
    uint8_t tmp = iowin_read8(&port_b, 0) & 0xFC;
    iowin_write8(&port_b, 0, tmp);
}

void speaker_beep(uint32_t freq, uint32_t ms)
{
    speaker_on(freq);
    timer_sleep(ms);
    speaker_off();
}

void speaker_boot_chime(void)
{
    speaker_beep(523, 80);
    timer_sleep(20);

    speaker_beep(659, 80);
    timer_sleep(20);

    speaker_beep(784, 120);
}

void speaker_panic_tone(void)
{
    for (int i = 0; i < 3; i++)
    {
        speaker_beep(220, 250);
        timer_sleep(100);
    }
}