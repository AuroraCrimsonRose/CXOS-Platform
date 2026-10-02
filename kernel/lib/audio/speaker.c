// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#include <stdint.h>

#include "speaker.h"
#include "io.h"
#include "timer.h"

#define PIT_COMMAND 0x43
#define PIT_CH2     0x42

static uint8_t speaker_enabled = 0;

void speaker_init(void)
{
    speaker_enabled = 1;
}

void speaker_on(uint32_t freq)
{
    if (!speaker_enabled) return;
    if (freq < 20) freq = 20;

    uint32_t divisor = 1193182u / freq;

    outb(PIT_COMMAND, 0xB6);

    outb(PIT_CH2, divisor & 0xFF);
    outb(PIT_CH2, (divisor >> 8) & 0xFF);

    uint8_t tmp = inb(0x61);

    if ((tmp & 3) != 3)
        outb(0x61, tmp | 3);
}

void speaker_off(void)
{
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
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