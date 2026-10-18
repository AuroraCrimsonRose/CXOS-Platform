// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ps2kbd.c */
/* Aurora Tejeda / CATX Systems */
/* PS/2 keyboard: IRQ 1 -> scancode (set 1) -> a key, handed to keyboard.c's
   buffer exactly as the USB HID driver hands it its keys. */

#include <stdint.h>
#include "ps2kbd.h"
#include "drivers/input/keyboard.h"   /* KEY_* and keyboard_inject */


/* --- US QWERTY scancode -> ASCII (set 1), unshifted and shifted --- */
/* index = scancode (press codes 0x00..0x58). 0 = no printable char. */
static const char map_lower[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0, ' ',
    /* rest zero */
};

static const char map_upper[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',
    0,  '*', 0, ' ',
};

/* modifier scancodes */
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CAPS   0x3A
#define SC_CTRL   0x1D   /* left control (and right, via 0xE0 prefix) */

/* extended-key scancodes (arrive after a 0xE0 prefix byte) */
#define SC_EXT_UP    0x48
#define SC_EXT_DOWN  0x50
#define SC_EXT_LEFT  0x4B
#define SC_EXT_RIGHT 0x4D
#define SC_EXT_PGUP  0x49
#define SC_EXT_PGDN  0x51
#define SC_EXT_HOME  0x47
#define SC_EXT_END   0x4F

unsigned ps2kbd_decode(struct ps2kbd_state *s, uint8_t sc) {
    /* 0xE0 = extended-key prefix; the next byte is the real (extended) code */
    if (sc == 0xE0) { s->ext = 1; return 0; }

    /* --- extended sequence (the byte right after a 0xE0) --- */
    if (s->ext) {
        s->ext = 0;

        /* Real keyboards wrap extended keys in "fake shift" codes
           (0xE0 0x2A / 0xE0 0xAA, etc.). Ignore those shift make/break bytes
           inside an extended sequence - they are not real shift events and
           must not disturb the shift tracking or consume the real code. */
        uint8_t code = sc & 0x7F;
        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            return 0;   /* swallow the fake shift; the real ext code follows */
        }

        /* extended key RELEASE (high bit set): just clear nothing, ignore */
        if (sc & 0x80) {
            if (code == SC_CTRL) s->ctrl = 0;   /* right-ctrl release */
            return 0;
        }

        /* extended key PRESS */
        switch (sc) {
            case SC_EXT_UP:    return s->shift ? KEY_SHIFT_UP : KEY_UP;
            case SC_EXT_DOWN:  return s->shift ? KEY_SHIFT_DN : KEY_DOWN;
            case SC_EXT_LEFT:  return KEY_LEFT;
            case SC_EXT_RIGHT: return KEY_RIGHT;
            case SC_EXT_PGUP:  return KEY_PGUP;
            case SC_EXT_PGDN:  return KEY_PGDN;
            case SC_EXT_HOME:  return KEY_HOME;
            case SC_EXT_END:   return KEY_END;
            case SC_CTRL:      s->ctrl = 1;        return 0;  /* right ctrl */
            default: return 0;
        }
    }

    /* --- non-extended key release: high bit set --- */
    if (sc & 0x80) {
        uint8_t code = sc & 0x7F;
        if (code == SC_LSHIFT || code == SC_RSHIFT) s->shift = 0;
        else if (code == SC_CTRL)                   s->ctrl = 0;
        return 0;
    }

    /* --- non-extended key press --- */
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) { s->shift = 1; return 0; }
    if (sc == SC_CAPS)                       { s->caps ^= 1;  return 0; }
    if (sc == SC_CTRL)                       { s->ctrl = 1; return 0; }

    if (sc >= 128) return 0;

    int upper = s->shift;
    char c = upper ? map_upper[sc] : map_lower[sc];

    /* Ctrl+letter -> the control code, 1-26 (Ctrl+A = 1 ... Ctrl+Z = 26).
       This used to special-case Ctrl+C alone, which left the PS/2 path
       disagreeing with the USB HID path - usb_hid.c has always done the
       general mapping, so Ctrl+S was 19 from a USB keyboard and a plain 's'
       from a PS/2 one. The USB driver exists on the promise that nothing above
       it can tell the two apart, so the general rule belongs here too. Ctrl+C
       still comes out as 3, which is what KEY_CTRL_C is. */
    if (s->ctrl && c >= 'a' && c <= 'z') return (unsigned char)(c - 'a' + 1);
    if (s->ctrl && c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 1);

    /* caps lock affects letters only */
    if (s->caps && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    else if (s->caps && s->shift && c >= 'A' && c <= 'Z') c = c - 'A' + 'a';

    return (unsigned char)c;
}

#ifndef CXK_HOST_TEST
#include "arch/irq.h"
#include "i8042.h"

static struct ps2kbd_state state;

/* IRQ1 handler */
static void ps2kbd_irq(struct registers *r) {
    (void)r;
    unsigned key = ps2kbd_decode(&state, i8042_data());
    if (key) keyboard_inject((unsigned char)key);
}

void ps2kbd_init(void) {
    irq_install_handler(1, ps2kbd_irq);
}
#endif
