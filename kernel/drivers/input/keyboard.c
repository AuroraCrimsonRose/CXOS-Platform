// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/keyboard.c */
/* Aurora Tejeda */
/* The keyboard as the rest of the kernel sees it: a buffer of keys, filled by
   whichever keyboard is attached (platform/pc/ps2kbd.c, usb_hid.c) through
   keyboard_inject, and read by the shell. */

#include <stdint.h>
#include "keyboard.h"
#include "usb_hid.h"
#include "../../cpu/sched.h"   /* thread_block / thread_unblock */

#define KBD_BUF_SIZE 128
static volatile unsigned char kbuf[KBD_BUF_SIZE];
static volatile int  khead = 0;   /* write index (producer: IRQ) */
static volatile int  ktail = 0;   /* read index  (consumer: shell) */

/* pid blocked waiting for a keystroke, or -1. One waiter is enough for now
   (a single shell/console owns the keyboard). The IRQ handler wakes it. */
static volatile int  kwaiter = -1;

static void kbuf_push(unsigned char c) {
    int next = (khead + 1) % KBD_BUF_SIZE;
    if (next != ktail) {          /* drop if full */
        kbuf[khead] = c;
        khead = next;
        /* wake a thread blocked in keyboard_getchar_blocking(). thread_unblock
           only flips state to READY - it does NOT context switch - so it is
           safe to call from an interrupt handler. */
        if (kwaiter >= 0) thread_unblock(kwaiter);
    }
}

void keyboard_inject(unsigned char c) { kbuf_push(c); }

/* How often to wake a blocked reader to re-poll USB, in timer ticks. The PIT
   runs at 1000 Hz, so 10 is 100 Hz - far more than a keyboard needs and cheap
   enough to be invisible. */
#define KBD_USB_POLL_TICKS 10
static unsigned usb_poll_div = 0;

void keyboard_tick(void) {
    /* Nothing to do without a USB HID device: the PS/2 path wakes its own
       waiter from IRQ1 and needs none of this. */
    if (!usb_hid_present()) return;
    if (kwaiter < 0) return;
    if (++usb_poll_div < KBD_USB_POLL_TICKS) return;
    usb_poll_div = 0;
    /* ONLY a wake. The USB transfer runs in the woken thread's own context -
       doing it here would put bus traffic inside an interrupt handler. */
    thread_unblock(kwaiter);
}

char keyboard_getchar(void) {
    /* Service USB HID here rather than from a timer: these transfers sleep, so
       they cannot run in interrupt context, and this is the point where somebody
       is asking for input anyway. Costs nothing when no HID device is attached. */
    usb_hid_poll();

    if (ktail == khead) return 0;          /* empty */
    char c = (char)kbuf[ktail];
    ktail = (ktail + 1) % KBD_BUF_SIZE;
    return c;
}

/* Block the CALLING THREAD until a key is available, using the scheduler's
   block/wake primitive (the same one IPC uses). This is safe from a syscall:
   the old implementation used `hlt`, which deadlocks when called through the
   int 0x80 gate (the gate clears IF, so no IRQ can ever wake the halt). Here
   the thread is descheduled instead, other threads run with interrupts on,
   and the keyboard's interrupt marks us runnable when a key lands. */
char keyboard_getchar_blocking(void) {
    for (;;) {
        char c = keyboard_getchar();
        if (c != 0) return c;

        kwaiter = thread_current_id();
        thread_block();          /* descheduled until a key arrives and wakes us */
        kwaiter = -1;
        if (thread_killed()) return 0;   /* no key: the thread ends on its way out */
    }
}

