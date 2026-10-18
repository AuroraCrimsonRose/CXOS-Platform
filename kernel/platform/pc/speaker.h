// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#pragma once

#include <stdint.h>

void speaker_init(void);

void speaker_on(uint32_t freq);
void speaker_off(void);

void speaker_beep(uint32_t freq, uint32_t ms);

void speaker_boot_chime(void);
void speaker_panic_tone(void);