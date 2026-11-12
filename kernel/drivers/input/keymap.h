// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/keymap.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Keys as one layer for every keyboard (roadmap 2.8a2). A driver says only
 * "this key (a USB HID usage, KC_*) went down" or "came up": usb_hid.c from
 * the difference between two reports, ps2kbd.c from its scancodes. This
 * keeps which keys are held, the modifiers and the locks, and gives back the
 * event (code, mods, the character under the US layout) and the character
 * the old key buffer has always held (KEY_* in keyboard.h, control codes for
 * Ctrl+letter), so the shell's line editor sees what it always did.
 *
 * The shape is Serenity's KeyEvent (Kernel/API/KeyCode.h: a key, the
 * code point, the modifier flags and press/release apart) with its rule
 * from InputManagement::get_char_from_character_map (Management.cpp): Caps
 * Lock turns letters over and nothing else. Pure: no locks, no I/O, so the
 * host tests run it.
 */

#ifndef DRIVERS_INPUT_KEYMAP_H
#define DRIVERS_INPUT_KEYMAP_H

#include <stdint.h>

struct keymap_state {
    uint32_t down[8];    /* a bit per usage, 0x00-0xFF: held now */
    uint32_t locks;      /* KEYMOD_CAPS | KEYMOD_NUM */
};

struct key_out {
    uint32_t code;       /* KC_* */
    uint32_t mods;       /* KEYMOD_* | KEYEV_* */
    uint32_t ch;         /* the character, printable, or 0 */
    unsigned legacy;     /* what the old buffer gets, or 0 for nothing */
    int attention;       /* Ctrl+Alt+Del: no event, no legacy key */
};

/* Num Lock starts on, as most firmware leaves it and as USB keyboards have
   always been read here (keypad digits). */
void keymap_init(struct keymap_state *s);

/* One key went down (down = 1) or up. Fills *out; returns 1 if it is an
   event to deliver, 0 if not (a release of a key not held, a usage out of
   range, or the attention combination, which sets out->attention). */
int keymap_feed(struct keymap_state *s, uint32_t usage, int down, struct key_out *out);

/* The modifiers and locks as they stand. */
uint32_t keymap_mods(const struct keymap_state *s);

/* For tests and keyboard_inject: the key and the held modifiers that type the
   old buffer's character c on a US keyboard. 0 if no key does. */
int keymap_from_legacy(unsigned c, uint32_t *usage, uint32_t *mods);

/* The press event and legacy key that keymap_feed would give for usage with
   mods held and locks as given, with no state: what keyboard_inject posts. */
void keymap_compose(uint32_t usage, uint32_t mods, struct key_out *out);

#endif
