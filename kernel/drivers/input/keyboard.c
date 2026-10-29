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
#include "../../cpu/sched.h"   /* thread_block_prepare / thread_unblock */
#include "arch/cpu_ops.h"

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

/* How long a blocked reader sleeps before it polls USB again: 100 Hz, far
   more than a keyboard needs and cheap enough to be invisible. A deadline of
   its own rather than a count of ticks, since without a fixed tick there is
   nothing to count. */
#define KBD_USB_POLL_MS 10

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

        /* Descheduled until a key arrives and wakes us, with interrupts held
           off from the look at the buffer to the block so a key landing
           between them is not slept through. With a USB HID device it also
           wakes to poll: the transfer runs here, in this thread, never in
           an interrupt handler. */
        uint32_t f = arch_irq_save();
        kwaiter = thread_current_id();
        int empty = (ktail == khead);
        if (empty) thread_block_prepare(usb_hid_present() ? KBD_USB_POLL_MS : 0);
        arch_irq_restore(f);
        if (empty) thread_block_finish();
        kwaiter = -1;
        if (thread_killed()) return 0;   /* no key: the thread ends on its way out */
    }
}

