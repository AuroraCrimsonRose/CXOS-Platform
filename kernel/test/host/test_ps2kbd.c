// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_ps2kbd.c */
/* Aurora Tejeda / CATX Systems */
/* The PS/2 scancode decoder (platform/pc/ps2kbd.c) and the key map behind
   it (drivers/input/keymap.c, 2.8a2): set-1 bytes in, HID usages between,
   the old buffer's keys and the events out. A USB keyboard feeds the same
   map with the same usages, so these are its keys too. */
#include "khost.h"
#include "platform/pc/ps2kbd.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/keymap.h"
#include "cxk_abi.h"

#include <string.h>

struct kb {
    struct ps2kbd_state p;
    struct keymap_state m;
    int ready;
    struct key_out ev[32];   /* the events of the last run */
    int nev;
};

/* Feed every byte and return the old buffer's keys, in order, 0-terminated;
   the attention combination is KEY_ATTENTION. Num Lock starts on. */
static const unsigned *run(struct kb *s, const uint8_t *sc, int n) {
    static unsigned out[32];
    int k = 0;
    memset(out, 0, sizeof out);
    if (!s->ready) { keymap_init(&s->m); s->ready = 1; }
    s->nev = 0;
    for (int i = 0; i < n; i++) {
        uint32_t u;
        int down;
        if (!ps2kbd_decode(&s->p, sc[i], &u, &down)) continue;
        struct key_out o;
        int ev = keymap_feed(&s->m, u, down, &o);
        if (o.attention && k < 31) out[k++] = KEY_ATTENTION;
        if (ev && s->nev < 32) s->ev[s->nev++] = o;
        if (ev && o.legacy && !(o.mods & KEYEV_RELEASE) && k < 31) out[k++] = o.legacy;
    }
    return out;
}
#define RUN(s, ...) run((s), (const uint8_t[]){ __VA_ARGS__ }, \
                        (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

KH_TEST(ps2kbd_plain_keys_and_releases) {
    struct kb s = { 0 };
    /* a, 1, space, enter, each pressed and released */
    const unsigned *o = RUN(&s, 0x1E, 0x9E, 0x02, 0x82, 0x39, 0xB9, 0x1C, 0x9C);
    KH_CHECK_EQ(o[0], 'a'); KH_CHECK_EQ(o[1], '1');
    KH_CHECK_EQ(o[2], ' '); KH_CHECK_EQ(o[3], '\n'); KH_CHECK_EQ(o[4], 0u);
}

KH_TEST(ps2kbd_shift_holds_until_released) {
    struct kb s = { 0 };
    /* left shift down: a, 1; released: a; right shift down: a */
    const unsigned *o = RUN(&s, 0x2A, 0x1E, 0x02, 0xAA, 0x1E, 0x36, 0x1E, 0xB6, 0x1E);
    KH_CHECK_EQ(o[0], 'A'); KH_CHECK_EQ(o[1], '!'); KH_CHECK_EQ(o[2], 'a');
    KH_CHECK_EQ(o[3], 'A'); KH_CHECK_EQ(o[4], 'a'); KH_CHECK_EQ(o[5], 0u);
}

KH_TEST(ps2kbd_caps_lock_is_letters_only_and_shift_inverts_it) {
    struct kb s = { 0 };
    /* caps on: a, 1, shift+a; caps off: a */
    const unsigned *o = RUN(&s, 0x3A, 0xBA, 0x1E, 0x02, 0x2A, 0x1E, 0xAA,
                                0x3A, 0xBA, 0x1E);
    KH_CHECK_EQ(o[0], 'A'); KH_CHECK_EQ(o[1], '1'); KH_CHECK_EQ(o[2], 'a');
    KH_CHECK_EQ(o[3], 'a'); KH_CHECK_EQ(o[4], 0u);
}

KH_TEST(ps2kbd_ctrl_letter_is_its_control_code) {
    struct kb s = { 0 };
    /* left ctrl: s, c, shift+a; released: s. right ctrl (E0 1D): s; released: s */
    const unsigned *o = RUN(&s, 0x1D, 0x1F, 0x2E, 0x2A, 0x1E, 0xAA, 0x9D, 0x1F,
                                0xE0, 0x1D, 0x1F, 0xE0, 0x9D, 0x1F);
    KH_CHECK_EQ(o[0], 19u); KH_CHECK_EQ(o[1], (unsigned)KEY_CTRL_C);
    KH_CHECK_EQ(o[2], 1u);  KH_CHECK_EQ(o[3], 's');
    KH_CHECK_EQ(o[4], 19u); KH_CHECK_EQ(o[5], 's'); KH_CHECK_EQ(o[6], 0u);
}

KH_TEST(ps2kbd_ctrl_alt_del_is_the_attention_key) {
    struct kb s = { 0 };
    /* Ctrl, Alt, the extended Delete */
    KH_CHECK_EQ(RUN(&s, 0x1D, 0x38, 0xE0, 0x53)[0], (unsigned)KEY_ATTENTION);
    /* held: its make code repeats and says nothing more */
    KH_CHECK_EQ(RUN(&s, 0xE0, 0x53, 0xE0, 0x53)[0], 0u);
    /* up and down again is another press */
    KH_CHECK_EQ(RUN(&s, 0xE0, 0xD3, 0xE0, 0x53)[0], (unsigned)KEY_ATTENTION);
    KH_CHECK_EQ(RUN(&s, 0xE0, 0xD3)[0], 0u);
    /* the keypad's Delete, and the right-hand Ctrl and Alt */
    struct kb r = { 0 };
    KH_CHECK_EQ(RUN(&r, 0xE0, 0x1D, 0xE0, 0x38, 0x53)[0], (unsigned)KEY_ATTENTION);
    /* the keypad's, released and pressed again, is another press */
    KH_CHECK_EQ(RUN(&r, 0xD3, 0x53)[0], (unsigned)KEY_ATTENTION);
}

KH_TEST(ps2kbd_attention_needs_all_three_held) {
    struct kb s = { 0 };
    KH_CHECK_EQ(RUN(&s, 0xE0, 0x53, 0xE0, 0xD3)[0], 0u);         /* Delete alone */
    KH_CHECK_EQ(RUN(&s, 0x1D, 0xE0, 0x53, 0xE0, 0xD3)[0], 0u);   /* Ctrl only */
    KH_CHECK_EQ(RUN(&s, 0x9D, 0x38, 0xE0, 0x53, 0xE0, 0xD3)[0], 0u); /* Alt only */
    /* Ctrl released before Delete */
    KH_CHECK_EQ(RUN(&s, 0x1D, 0x9D, 0xE0, 0x53, 0xE0, 0xD3)[0], 0u);
    /* Alt released (the left one, held since "Alt only") */
    KH_CHECK_EQ(RUN(&s, 0x1D, 0xB8, 0xE0, 0x53, 0xE0, 0xD3)[0], 0u);
    /* each Alt is its own key: the right one's break code does not let go
       of the left (the old decoder kept one flag for both) */
    KH_CHECK_EQ(RUN(&s, 0x38, 0xE0, 0xB8, 0xE0, 0x53)[0], (unsigned)KEY_ATTENTION);
}

KH_TEST(ps2kbd_extended_keys) {
    struct kb s = { 0 };
    const unsigned *o = RUN(&s, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0x50, 0xE0, 0x4B,
                                0xE0, 0x4D, 0xE0, 0x49, 0xE0, 0x51, 0xE0, 0x47,
                                0xE0, 0x4F);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP);   KH_CHECK_EQ(o[1], (unsigned)KEY_DOWN);
    KH_CHECK_EQ(o[2], (unsigned)KEY_LEFT); KH_CHECK_EQ(o[3], (unsigned)KEY_RIGHT);
    KH_CHECK_EQ(o[4], (unsigned)KEY_PGUP); KH_CHECK_EQ(o[5], (unsigned)KEY_PGDN);
    KH_CHECK_EQ(o[6], (unsigned)KEY_HOME); KH_CHECK_EQ(o[7], (unsigned)KEY_END);
    KH_CHECK_EQ(o[8], 0u);
    /* the prefix covers one byte only: a bare 0x48 after it is keypad 8,
       a digit with Num Lock on (2.8a2: it had no key here before) */
    o = RUN(&s, 0xE0, 0x48, 0x48);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP); KH_CHECK_EQ(o[1], '8'); KH_CHECK_EQ(o[2], 0u);
    KH_CHECK_EQ(s.ev[1].code, 0x60u);
    /* Num Lock off: the keypad is the arrows, and its code says so */
    o = RUN(&s, 0xC8, 0x45, 0xC5, 0x48, 0xC8, 0x47);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP); KH_CHECK_EQ(o[1], (unsigned)KEY_HOME); KH_CHECK_EQ(o[2], 0u);
    KH_CHECK_EQ(s.ev[3].code, KC_UP);
    KH_CHECK_EQ(s.ev[3].mods & KEYMOD_NUM, 0u);
}

KH_TEST(ps2kbd_shift_arrows_scroll) {
    struct kb s = { 0 };
    const unsigned *o = RUN(&s, 0x2A, 0xE0, 0x48, 0xE0, 0x50, 0xAA, 0xE0, 0x48);
    KH_CHECK_EQ(o[0], (unsigned)KEY_SHIFT_UP); KH_CHECK_EQ(o[1], (unsigned)KEY_SHIFT_DN);
    KH_CHECK_EQ(o[2], (unsigned)KEY_UP);       KH_CHECK_EQ(o[3], 0u);
}

/* A real keyboard with Num Lock on wraps an arrow in fake shift codes:
   E0 2A E0 48 ... E0 C8 E0 AA. They are not the user's shift. */
KH_TEST(ps2kbd_fake_shift_is_not_shift) {
    struct kb s = { 0 };
    const unsigned *o = RUN(&s, 0xE0, 0x2A, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0xAA, 0x1E);
    KH_CHECK_EQ(o[0], (unsigned)KEY_UP); KH_CHECK_EQ(o[1], 'a'); KH_CHECK_EQ(o[2], 0u);
    /* and a real shift held across one survives its fake release */
    o = RUN(&s, 0x2A, 0xE0, 0xAA, 0xE0, 0x48, 0x1E, 0xAA);
    KH_CHECK_EQ(o[0], (unsigned)KEY_SHIFT_UP); KH_CHECK_EQ(o[1], 'A'); KH_CHECK_EQ(o[2], 0u);
}

/* ---- the events (2.8a2) ---- */

KH_TEST(keymap_press_and_release_are_events_with_code_mods_and_char) {
    struct kb s = { 0 };
    /* shift down, a down, a up, shift up */
    RUN(&s, 0x2A, 0x1E, 0x9E, 0xAA);
    KH_CHECK_EQ(s.nev, 4);
    KH_CHECK_EQ(s.ev[0].code, KC_LSHIFT); KH_CHECK_EQ(s.ev[0].mods & KEYMOD_HELD, KEYMOD_SHIFT);
    KH_CHECK_EQ(s.ev[0].ch, 0u);
    KH_CHECK_EQ(s.ev[1].code, KC_A); KH_CHECK_EQ(s.ev[1].ch, 'A');
    KH_CHECK_EQ(s.ev[1].mods & (KEYMOD_HELD | KEYEV_RELEASE | KEYEV_REPEAT), KEYMOD_SHIFT);
    KH_CHECK_EQ(s.ev[2].code, KC_A); KH_CHECK_EQ(s.ev[2].ch, 0u);
    KH_CHECK_EQ(s.ev[2].mods & KEYEV_RELEASE, KEYEV_RELEASE);
    KH_CHECK_EQ(s.ev[3].code, KC_LSHIFT); KH_CHECK_EQ(s.ev[3].mods & (KEYMOD_HELD | KEYEV_RELEASE), KEYEV_RELEASE);
}

KH_TEST(keymap_ctrl_keeps_the_letter_in_the_event) {
    struct kb s = { 0 };
    const unsigned *o = RUN(&s, 0x1D, 0x2E);
    KH_CHECK_EQ(o[0], 3u);                       /* the old buffer: ETX */
    KH_CHECK_EQ(s.ev[1].code, KC_C);
    KH_CHECK_EQ(s.ev[1].ch, 'c');
    KH_CHECK_EQ(s.ev[1].mods & KEYMOD_HELD, KEYMOD_CTRL);
}

KH_TEST(keymap_a_held_key_repeats_and_a_held_modifier_does_not) {
    struct kb s = { 0 };
    const unsigned *o = RUN(&s, 0x2A, 0x2A, 0x2A, 0x1E, 0x1E, 0x1E);
    KH_CHECK_EQ(s.nev, 4);                       /* shift once, a three times */
    KH_CHECK_EQ(s.ev[1].mods & KEYEV_REPEAT, 0u);
    KH_CHECK_EQ(s.ev[2].mods & KEYEV_REPEAT, KEYEV_REPEAT);
    KH_CHECK_EQ(s.ev[3].mods & KEYEV_REPEAT, KEYEV_REPEAT);
    KH_CHECK_EQ(o[0], 'A'); KH_CHECK_EQ(o[1], 'A'); KH_CHECK_EQ(o[2], 'A');
    /* a release of a key never pressed is nothing */
    RUN(&s, 0x9F);
    KH_CHECK_EQ(s.nev, 0);
}

KH_TEST(keymap_super_menu_f_keys_and_pause) {
    struct kb s = { 0 };
    const unsigned *o = RUN(&s, 0xE0, 0x5B, 0x26, 0xA6, 0xE0, 0xDB, 0x3C, 0xBC,
                                0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5);
    KH_CHECK_EQ(s.ev[0].code, KC_LSUPER);
    KH_CHECK_EQ(s.ev[1].code, KC_L); KH_CHECK_EQ(s.ev[1].mods & KEYMOD_HELD, KEYMOD_SUPER);
    KH_CHECK_EQ(s.ev[4].code, KC_F2);
    KH_CHECK_EQ(s.ev[6].code, KC_PAUSE); KH_CHECK_EQ(s.ev[6].mods & KEYEV_RELEASE, 0u);
    KH_CHECK_EQ(s.ev[7].code, KC_PAUSE); KH_CHECK_EQ(s.ev[7].mods & KEYEV_RELEASE, KEYEV_RELEASE);
    KH_CHECK_EQ(s.nev, 8);                       /* Pause's middle bytes are not Ctrl */
    KH_CHECK_EQ(o[0], 'l'); KH_CHECK_EQ(o[1], 0u);
}

KH_TEST(keymap_caps_lock_is_in_mods_and_turns_letters_only) {
    struct kb s = { 0 };
    RUN(&s, 0x3A, 0xBA, 0x1E, 0x02);
    KH_CHECK_EQ(s.ev[2].ch, 'A');
    KH_CHECK_EQ(s.ev[3].ch, '1');
    KH_CHECK_EQ(s.ev[3].mods & KEYMOD_CAPS, KEYMOD_CAPS);
}

KH_TEST(keymap_every_old_key_has_a_press_that_types_it) {
    /* what keyboard_inject relies on: from_legacy, then compose, is c again */
    unsigned cs[] = { '\b', '\t', '\n', 0x1B, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
                      KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, KEY_SHIFT_UP, KEY_SHIFT_DN };
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        uint32_t u, m;
        struct key_out o;
        KH_CHECK_EQ(keymap_from_legacy(cs[i], &u, &m), 1);
        keymap_compose(u, m, &o);
        KH_CHECK_EQ(o.legacy, cs[i]);
    }
    for (unsigned c = 1; c < 0x7F; c++) {
        uint32_t u, m;
        struct key_out o;
        if (c == '\r' || (c > 26 && c < 0x20 && c != 0x1B)) continue;
        KH_CHECK_EQ(keymap_from_legacy(c, &u, &m), 1);
        keymap_compose(u, m, &o);
        KH_CHECK_EQ(o.legacy, c);
        if (c >= 0x20) KH_CHECK_EQ(o.ch, c);
    }
}
