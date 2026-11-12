// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ps2kbd.c */
/* Aurora Tejeda / CATX Systems */
/* PS/2 keyboard: IRQ 1 -> scancode (set 1) -> a HID usage, down or up,
   handed to keyboard.c exactly as the USB HID driver hands it its keys
   (2.8a2: what the keys mean is drivers/input/keymap.c's, for both). */

#include <stdint.h>
#include "ps2kbd.h"
#include "drivers/input/keyboard.h"   /* keyboard_key */
#include "cxk_abi.h"                  /* KC_* */

/* Set 1 make code (0x01-0x58) -> usage. 0 = no key. */
static const uint8_t set1_usage[0x59] = {
    /* 0x00 */ 0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
    /* 0x08 */ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
    /* 0x10 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C,
    /* 0x18 */ 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,
    /* 0x20 */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33,
    /* 0x28 */ 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,
    /* 0x30 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55,
    /* 0x38 */ 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    /* 0x40 */ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F,
    /* 0x48 */ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
    /* 0x50 */ 0x5A, 0x5B, 0x62, 0x63, 0,    0,    0x64, 0x44,
    /* 0x58 */ 0x45,
};

/* After 0xE0. */
static uint8_t ext_usage(uint8_t code) {
    switch (code) {
        case 0x1C: return KC_KP_ENTER;
        case 0x1D: return KC_RCTRL;
        case 0x35: return 0x54;          /* keypad / */
        case 0x37: return KC_PRINT;
        case 0x38: return KC_RALT;
        case 0x47: return KC_HOME;
        case 0x48: return KC_UP;
        case 0x49: return KC_PGUP;
        case 0x4B: return KC_LEFT;
        case 0x4D: return KC_RIGHT;
        case 0x4F: return KC_END;
        case 0x50: return KC_DOWN;
        case 0x51: return KC_PGDN;
        case 0x52: return KC_INSERT;
        case 0x53: return KC_DELETE;
        case 0x5B: return KC_LSUPER;
        case 0x5C: return KC_RSUPER;
        case 0x5D: return KC_MENU;
        default:   return 0;
    }
}

int ps2kbd_decode(struct ps2kbd_state *s, uint8_t sc, uint32_t *usage, int *down) {
    /* Pause is E1 1D 45 E1 9D C5 and has no release of its own: the first
       byte is its press, the last its release, the four between nothing */
    if (s->e1) {
        s->e1--;
        if (s->e1 == 0) { *usage = KC_PAUSE; *down = 0; return 1; }
        return 0;
    }
    if (sc == 0xE1) { s->e1 = 5; *usage = KC_PAUSE; *down = 1; return 1; }
    /* 0xE0 = extended-key prefix; the next byte is the real (extended) code */
    if (sc == 0xE0) { s->ext = 1; return 0; }
    uint8_t code = sc & 0x7F;
    int press = (sc & 0x80) == 0;
    uint8_t u;
    if (s->ext) {
        s->ext = 0;
        /* Real keyboards wrap extended keys in "fake shift" codes
           (0xE0 0x2A / 0xE0 0xAA, etc.). They are not the user's shift and
           must not disturb it or consume the real code. */
        if (code == 0x2A || code == 0x36) return 0;
        u = ext_usage(code);
    } else {
        u = code < sizeof set1_usage ? set1_usage[code] : 0;
    }
    if (!u) return 0;
    *usage = u;
    *down = press;
    return 1;
}

#ifndef CXK_HOST_TEST
#include "arch/irq.h"
#include "i8042.h"

static struct ps2kbd_state state;

/* IRQ1 handler */
static void ps2kbd_irq(struct registers *r) {
    (void)r;
    uint32_t usage;
    int down;
    if (ps2kbd_decode(&state, i8042_data(), &usage, &down)) keyboard_key(usage, down);
}

void ps2kbd_init(void) {
    irq_install_handler(1, ps2kbd_irq);
}
#endif
