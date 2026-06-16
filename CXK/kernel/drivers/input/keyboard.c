/* /CXLite/kernel/drivers/keyboard.c */
/* Aurora Tejeda */
/* PS/2 keyboard driver: IRQ1 -> scancode -> ASCII -> circular buffer. */

#include <stdint.h>
#include "keyboard.h"
#include "idt.h"

#define KBD_DATA_PORT 0x60

static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

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

static int shift_down = 0;
static int caps_on = 0;
static int ctrl_down = 0;
static int ext_pending = 0;   /* set after a 0xE0 prefix byte */

#define KBD_BUF_SIZE 128
static volatile unsigned char kbuf[KBD_BUF_SIZE];
static volatile int  khead = 0;   /* write index (producer: IRQ) */
static volatile int  ktail = 0;   /* read index  (consumer: shell) */

static void kbuf_push(unsigned char c) {
    int next = (khead + 1) % KBD_BUF_SIZE;
    if (next != ktail) {          /* drop if full */
        kbuf[khead] = c;
        khead = next;
    }
}

char keyboard_getchar(void) {
    if (ktail == khead) return 0;          /* empty */
    char c = (char)kbuf[ktail];
    ktail = (ktail + 1) % KBD_BUF_SIZE;
    return c;
}

char keyboard_getchar_blocking(void) {
    char c;
    while ((c = keyboard_getchar()) == 0) {
        __asm__ volatile ("hlt");          /* idle until next interrupt */
    }
    return c;
}

/* IRQ1 handler */
static void keyboard_callback(struct registers *r) {
    (void)r;
    uint8_t sc = inb(KBD_DATA_PORT);

    /* 0xE0 = extended-key prefix; the next byte is the real (extended) code */
    if (sc == 0xE0) { ext_pending = 1; return; }

    /* --- extended sequence (the byte right after a 0xE0) --- */
    if (ext_pending) {
        ext_pending = 0;

        /* Real keyboards wrap extended keys in "fake shift" codes
           (0xE0 0x2A / 0xE0 0xAA, etc.). Ignore those shift make/break bytes
           inside an extended sequence - they are not real shift events and
           must not disturb our shift_down tracking or consume the real code. */
        uint8_t code = sc & 0x7F;
        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            return;   /* swallow the fake shift; the real ext code follows */
        }

        /* extended key RELEASE (high bit set): just clear nothing, ignore */
        if (sc & 0x80) {
            if (code == SC_CTRL) ctrl_down = 0;   /* right-ctrl release */
            return;
        }

        /* extended key PRESS */
        switch (sc) {
            case SC_EXT_UP:    kbuf_push(shift_down ? KEY_SHIFT_UP : KEY_UP);   return;
            case SC_EXT_DOWN:  kbuf_push(shift_down ? KEY_SHIFT_DN : KEY_DOWN); return;
            case SC_EXT_LEFT:  kbuf_push(KEY_LEFT);  return;
            case SC_EXT_RIGHT: kbuf_push(KEY_RIGHT); return;
            case SC_EXT_PGUP:  kbuf_push(KEY_PGUP);  return;
            case SC_EXT_PGDN:  kbuf_push(KEY_PGDN);  return;
            case SC_EXT_HOME:  kbuf_push(KEY_HOME);  return;
            case SC_EXT_END:   kbuf_push(KEY_END);   return;
            case SC_CTRL:      ctrl_down = 1;        return;  /* right ctrl */
            default: return;
        }
    }

    /* --- non-extended key release: high bit set --- */
    if (sc & 0x80) {
        uint8_t code = sc & 0x7F;
        if (code == SC_LSHIFT || code == SC_RSHIFT) shift_down = 0;
        else if (code == SC_CTRL)                   ctrl_down = 0;
        return;
    }

    /* --- non-extended key press --- */
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift_down = 1; return; }
    if (sc == SC_CAPS)                       { caps_on ^= 1;  return; }
    if (sc == SC_CTRL)                       { ctrl_down = 1; return; }

    if (sc >= 128) return;

    int upper = shift_down;
    char c = upper ? map_upper[sc] : map_lower[sc];

    /* Ctrl+C -> abort code (before normal char handling) */
    if (ctrl_down && (c == 'c' || c == 'C')) { kbuf_push(KEY_CTRL_C); return; }

    /* caps lock affects letters only */
    if (caps_on && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    else if (caps_on && shift_down && c >= 'A' && c <= 'Z') c = c - 'A' + 'a';

    if (c) kbuf_push((unsigned char)c);
}

void keyboard_init(void) {
    irq_install_handler(1, keyboard_callback);
}