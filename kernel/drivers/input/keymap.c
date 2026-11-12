// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/keymap.c */
/* Aurora Tejeda / CATX Systems */
/* Keys for every keyboard, from HID usages (keymap.h says why and how). */

#include <stdint.h>
#include "keymap.h"
#include "keyboard.h"   /* KEY_* */
#include "cxk_abi.h"    /* KC_*, KEYMOD_*, KEYEV_* */

/* ---- US QWERTY, by usage: unshifted and shifted. 0 = no character. ---- */
#define MAP_N 0x65
static const char us_lower[MAP_N] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
               'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 */ '\n', 0x1B, '\b', '\t', ' ',
    /* 0x2D */ '-','=','[',']','\\',
    /* 0x32 */ 0,           /* non-US # */
    /* 0x33 */ ';','\'','`',',','.','/',
    /* 0x39 */ 0,           /* caps lock */
    /* 0x3A */ 0,0,0,0,0,0,0,0,0,0,0,0,   /* F1-F12 */
    /* 0x46 */ 0,0,0,       /* print screen, scroll lock, pause */
    /* 0x49 */ 0,0,0,0,0,0,0,0,0,0,       /* insert .. up */
    /* 0x53 */ 0,           /* num lock */
    /* 0x54 */ '/','*','-','+','\n',
    /* 0x59 */ '1','2','3','4','5','6','7','8','9','0','.',
    /* 0x64 */ '\\',        /* non-US backslash */
};

static const char us_upper[MAP_N] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'A','B','C','D','E','F','G','H','I','J','K','L','M',
               'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    /* 0x1E */ '!','@','#','$','%','^','&','*','(',')',
    /* 0x28 */ '\n', 0x1B, '\b', '\t', ' ',
    /* 0x2D */ '_','+','{','}','|',
    /* 0x32 */ 0,
    /* 0x33 */ ':','"','~','<','>','?',
    /* 0x39 */ 0,
    /* 0x3A */ 0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x46 */ 0,0,0,
    /* 0x49 */ 0,0,0,0,0,0,0,0,0,0,
    /* 0x53 */ 0,
    /* 0x54 */ '/','*','-','+','\n',
    /* 0x59 */ '1','2','3','4','5','6','7','8','9','0','.',
    /* 0x64 */ '|',
};

#define KP_1   0x59
#define KP_0   0x62

static int is_letter(uint32_t u) { return u >= KC_A && u <= KC_Z; }

static uint32_t mod_bit(uint32_t u) {
    switch (u) {
        case KC_LSHIFT: case KC_RSHIFT: return KEYMOD_SHIFT;
        case KC_LCTRL:  case KC_RCTRL:  return KEYMOD_CTRL;
        case KC_LALT:   case KC_RALT:   return KEYMOD_ALT;
        case KC_LSUPER: case KC_RSUPER: return KEYMOD_SUPER;
        default: return 0;
    }
}

/* With Num Lock off the keypad is the navigation keys, and says so in its
   code: a program asks for KC_UP, not for "keypad 8 while Num Lock is off". */
static uint32_t keypad_nav(uint32_t u) {
    switch (u) {
        case 0x5F: return KC_HOME;   case 0x60: return KC_UP;
        case 0x61: return KC_PGUP;   case 0x5C: return KC_LEFT;
        case 0x5E: return KC_RIGHT;  case KP_1: return KC_END;
        case 0x5A: return KC_DOWN;   case 0x5B: return KC_PGDN;
        case KP_0: return KC_INSERT; case KC_KP_PERIOD: return KC_DELETE;
        default:   return 0;
    }
}

static unsigned legacy_nav(uint32_t code, uint32_t held) {
    int shift = (held & KEYMOD_SHIFT) != 0;
    switch (code) {
        case KC_HOME:  return KEY_HOME;
        case KC_PGUP:  return KEY_PGUP;
        case KC_END:   return KEY_END;
        case KC_PGDN:  return KEY_PGDN;
        case KC_RIGHT: return KEY_RIGHT;
        case KC_LEFT:  return KEY_LEFT;
        case KC_DOWN:  return shift ? KEY_SHIFT_DN : KEY_DOWN;
        case KC_UP:    return shift ? KEY_SHIFT_UP : KEY_UP;
        default:       return 0;
    }
}

/* What a press of usage gives with these modifiers and locks. */
static void compose(uint32_t usage, uint32_t held, uint32_t locks, struct key_out *o) {
    uint32_t code = usage;
    if ((locks & KEYMOD_NUM) == 0 && usage >= KP_1 && usage <= KC_KP_PERIOD) {
        uint32_t nav = keypad_nav(usage);
        if (nav) code = nav;
    }
    o->code = code;
    o->mods = held | locks;
    o->ch = 0;
    o->legacy = 0;
    o->attention = 0;
    if (mod_bit(usage) || usage == KC_CAPS || usage == KC_NUMLOCK) return;
    unsigned nav = legacy_nav(code, held);
    if (nav) { o->legacy = nav; return; }
    if (code >= MAP_N) return;
    int shift = (held & KEYMOD_SHIFT) != 0;
    char c = shift ? us_upper[code] : us_lower[code];
    if (!c) return;
    /* Caps Lock turns letters over and nothing else */
    if (is_letter(code) && (locks & KEYMOD_CAPS)) c = shift ? us_lower[code] : us_upper[code];
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) o->ch = (unsigned char)c;
    o->legacy = (unsigned char)c;
    /* Ctrl+letter is the control code in the old buffer, as it always was
       from both drivers (Ctrl+C is 3); the event keeps the letter */
    if ((held & KEYMOD_CTRL) && is_letter(code)) o->legacy = code - KC_A + 1;
}

void keymap_init(struct keymap_state *s) {
    for (int i = 0; i < 8; i++) s->down[i] = 0;
    s->locks = KEYMOD_NUM;
}

static uint32_t held_of(const struct keymap_state *s) {
    uint32_t m = 0;
    for (uint32_t u = KC_LCTRL; u <= KC_RSUPER; u++)
        if (s->down[u >> 5] & (1u << (u & 31))) m |= mod_bit(u);
    return m;
}

uint32_t keymap_mods(const struct keymap_state *s) { return held_of(s) | s->locks; }

int keymap_feed(struct keymap_state *s, uint32_t usage, int down, struct key_out *out) {
    if (usage > 0xFF || usage < KC_A) return 0;
    uint32_t bit = 1u << (usage & 31);
    uint32_t *w = &s->down[usage >> 5];
    int was = (*w & bit) != 0;
    if (!down) {
        if (!was) return 0;          /* nothing held to let go of */
        *w &= ~bit;
        compose(usage, held_of(s), s->locks, out);
        out->mods |= KEYEV_RELEASE;
        out->ch = 0;
        out->legacy = 0;
        return 1;
    }
    *w |= bit;
    if (was && (mod_bit(usage) || usage == KC_CAPS || usage == KC_NUMLOCK)) return 0;  /* a held modifier's typematic */
    if (!was && usage == KC_CAPS) s->locks ^= KEYMOD_CAPS;
    if (!was && usage == KC_NUMLOCK) s->locks ^= KEYMOD_NUM;
    uint32_t held = held_of(s);
    compose(usage, held, s->locks, out);
    if ((usage == KC_DELETE || usage == KC_KP_PERIOD)
        && (held & (KEYMOD_CTRL | KEYMOD_ALT)) == (KEYMOD_CTRL | KEYMOD_ALT)) {
        /* the combination, once a press however long it is held */
        out->legacy = 0;
        out->attention = !was;
        return 0;
    }
    if (was) out->mods |= KEYEV_REPEAT;
    return 1;
}

void keymap_compose(uint32_t usage, uint32_t mods, struct key_out *out) {
    compose(usage, mods & KEYMOD_HELD, KEYMOD_NUM, out);
}

int keymap_from_legacy(unsigned c, uint32_t *usage, uint32_t *mods) {
    *mods = 0;
    switch (c) {
        case '\b': *usage = KC_BACKSPACE; return 1;
        case '\t': *usage = KC_TAB; return 1;
        case '\n': *usage = KC_ENTER; return 1;
        case 0x1B: *usage = KC_ESC; return 1;
        case KEY_UP:    *usage = KC_UP; return 1;
        case KEY_DOWN:  *usage = KC_DOWN; return 1;
        case KEY_LEFT:  *usage = KC_LEFT; return 1;
        case KEY_RIGHT: *usage = KC_RIGHT; return 1;
        case KEY_PGUP:  *usage = KC_PGUP; return 1;
        case KEY_PGDN:  *usage = KC_PGDN; return 1;
        case KEY_HOME:  *usage = KC_HOME; return 1;
        case KEY_END:   *usage = KC_END; return 1;
        case KEY_SHIFT_UP: *usage = KC_UP;   *mods = KEYMOD_SHIFT; return 1;
        case KEY_SHIFT_DN: *usage = KC_DOWN; *mods = KEYMOD_SHIFT; return 1;
        default: break;
    }
    if (c >= 1 && c <= 26) { *usage = KC_A + c - 1; *mods = KEYMOD_CTRL; return 1; }
    /* the main block before the keypad, so '1' is the 1 key */
    for (uint32_t u = KC_A; u < MAP_N; u++) {
        if ((unsigned char)us_lower[u] == c) { *usage = u; return 1; }
        if ((unsigned char)us_upper[u] == c) { *usage = u; *mods = KEYMOD_SHIFT; return 1; }
    }
    return 0;
}
