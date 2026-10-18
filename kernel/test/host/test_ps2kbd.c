// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_ps2kbd.c */
/* Aurora Tejeda / CATX Systems */
/* The PS/2 scancode decoder (platform/pc/ps2kbd.c): set-1 bytes in, the keys
   keyboard.c is handed out. A USB keyboard must be indistinguishable from it,
   so these are the keys usb_hid.c produces for the same presses. */
#include "khost.h"
#include "platform/pc/ps2kbd.h"
#include "drivers/input/keyboard.h"

#include <string.h>

/* Feed every byte and return the keys produced, in order, 0-terminated. */
static const unsigned *run(struct ps2kbd_state *s, const uint8_t *sc, int n) {
    static unsigned out[32];
    int k = 0;
    memset(out, 0, sizeof out);
    for (int i = 0; i < n; i++) {
        unsigned key = ps2kbd_decode(s, sc[i]);
        if (key && k < 31) out[k++] = key;
    }
    return out;
}
#define RUN(s, ...) run((s), (const uint8_t[]){ __VA_ARGS__ }, \
                        (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

KH_TEST(ps2kbd_plain_keys_and_releases) {
    struct ps2kbd_state s = {0};
    /* a, 1, space, enter, each pressed and released */
    const unsigned *o = RUN(&s, 0x1E, 0x9E, 0x02, 0x82, 0x39, 0xB9, 0x1C, 0x9C);
    KH_CHECK_EQ(o[0], 'a'); KH_CHECK_EQ(o[1], '1');
    KH_CHECK_EQ(o[2], ' '); KH_CHECK_EQ(o[3], '\n'); KH_CHECK_EQ(o[4], 0u);
}

KH_TEST(ps2kbd_shift_holds_until_released) {
    struct ps2kbd_state s = {0};
    /* left shift down: a, 1; released: a; right shift down: a */
    const unsigned *o = RUN(&s, 0x2A, 0x1E, 0x02, 0xAA, 0x1E, 0x36, 0x1E, 0xB6, 0x1E);
    KH_CHECK_EQ(o[0], 'A'); KH_CHECK_EQ(o[1], '!'); KH_CHECK_EQ(o[2], 'a');
    KH_CHECK_EQ(o[3], 'A'); KH_CHECK_EQ(o[4], 'a'); KH_CHECK_EQ(o[5], 0u);
}

KH_TEST(ps2kbd_caps_lock_is_letters_only_and_shift_inverts_it) {
    struct ps2kbd_state s = {0};
    /* caps on: a, 1, shift+a; caps off: a */
    const unsigned *o = RUN(&s, 0x3A, 0xBA, 0x1E, 0x02, 0x2A, 0x1E, 0xAA,
                                0x3A, 0xBA, 0x1E);
    KH_CHECK_EQ(o[0], 'A'); KH_CHECK_EQ(o[1], '1'); KH_CHECK_EQ(o[2], 'a');
    KH_CHECK_EQ(o[3], 'a'); KH_CHECK_EQ(o[4], 0u);
}

KH_TEST(ps2kbd_ctrl_letter_is_its_control_code) {
    struct ps2kbd_state s = {0};
    /* left ctrl: s, c, shift+a; released: s. right ctrl (E0 1D): s; released: s */
    const unsigned *o = RUN(&s, 0x1D, 0x1F, 0x2E, 0x2A, 0x1E, 0xAA, 0x9D, 0x1F,
                                0xE0, 0x1D, 0x1F, 0xE0, 0x9D, 0x1F);
    KH_CHECK_EQ(o[0], 19u); KH_CHECK_EQ(o[1], (unsigned)KEY_CTRL_C);
    KH_CHECK_EQ(o[2], 1u);  KH_CHECK_EQ(o[3], 's');
    KH_CHECK_EQ(o[4], 19u); KH_CHECK_EQ(o[5], 's'); KH_CHECK_EQ(o[6], 0u);
}

KH_TEST(ps2kbd_extended_keys) {
    struct ps2kbd_state s = {0};
    const unsigned *o = RUN(&s, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0x50, 0xE0, 0x4B,
                                0xE0, 0x4D, 0xE0, 0x49, 0xE0, 0x51, 0xE0, 0x47,
                                0xE0, 0x4F);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP);   KH_CHECK_EQ(o[1], (unsigned)KEY_DOWN);
    KH_CHECK_EQ(o[2], (unsigned)KEY_LEFT); KH_CHECK_EQ(o[3], (unsigned)KEY_RIGHT);
    KH_CHECK_EQ(o[4], (unsigned)KEY_PGUP); KH_CHECK_EQ(o[5], (unsigned)KEY_PGDN);
    KH_CHECK_EQ(o[6], (unsigned)KEY_HOME); KH_CHECK_EQ(o[7], (unsigned)KEY_END);
    KH_CHECK_EQ(o[8], 0u);
    /* the prefix covers one byte only: a bare 0x48 after it is keypad 8,
       which has no key here */
    o = RUN(&s, 0xE0, 0x48, 0x48);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP); KH_CHECK_EQ(o[1], 0u);
}

KH_TEST(ps2kbd_shift_arrows_scroll) {
    struct ps2kbd_state s = {0};
    const unsigned *o = RUN(&s, 0x2A, 0xE0, 0x48, 0xE0, 0x50, 0xAA, 0xE0, 0x48);
    KH_CHECK_EQ(o[0], (unsigned)KEY_SHIFT_UP); KH_CHECK_EQ(o[1], (unsigned)KEY_SHIFT_DN);
    KH_CHECK_EQ(o[2], (unsigned)KEY_UP);       KH_CHECK_EQ(o[3], 0u);
}

/* A real keyboard with Num Lock on wraps an arrow in fake shift codes:
   E0 2A E0 48 ... E0 C8 E0 AA. They are not the user's shift. */
KH_TEST(ps2kbd_fake_shift_is_not_shift) {
    struct ps2kbd_state s = {0};
    const unsigned *o = RUN(&s, 0xE0, 0x2A, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0xAA, 0x1E);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP); KH_CHECK_EQ(o[1], 'a'); KH_CHECK_EQ(o[2], 0u);
    /* and a real shift held across one survives its fake release */
    o = RUN(&s, 0x2A, 0xE0, 0xAA, 0xE0, 0x48, 0x1E, 0xAA);
    KH_CHECK_EQ(o[0], (unsigned)KEY_SHIFT_UP); KH_CHECK_EQ(o[1], 'A'); KH_CHECK_EQ(o[2], 0u);
}
