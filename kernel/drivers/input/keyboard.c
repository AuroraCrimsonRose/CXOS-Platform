// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/keyboard.c */
/* Aurora Tejeda */
/* The keyboard as the rest of the kernel sees it: a buffer of keys, filled by
   whichever keyboard is attached (platform/pc/ps2kbd.c, usb_hid.c) through
   keyboard_inject, and read by the shell. */

#include <stdint.h>
#include "keyboard.h"
#include "inputdev.h"
#include "usb_hid.h"
#include "../../cpu/sched.h"   /* thread_block_prepare / thread_unblock */
#include "arch/cpu_ops.h"
#include "sync/spinlock.h"

#define KBD_BUF_SIZE 128
static volatile unsigned char kbuf[KBD_BUF_SIZE];
static volatile int  khead = 0;   /* write index (producer: IRQ) */
static volatile int  ktail = 0;   /* read index  (consumer: shell) */

/* pid blocked waiting for a keystroke, or -1. One waiter is enough for now
   (a single shell/console owns the keyboard). The IRQ handler wakes it. */
static volatile int  kwaiter = -1;

/* The buffer, its indexes and the waiter (1.9e4): the interrupt fills it on
   the boot CPU while a reader on another CPU empties it, and two readers
   would otherwise take one key. A reader prepares its block under the lock,
   so a key pushed between its look and its block wakes it. */
static spinlock_t kbd_lock = SPINLOCK_INIT(LR_INPUT);

static void kbuf_push(unsigned char c) {
    uint32_t f = spin_lock(&kbd_lock);
    int next = (khead + 1) % KBD_BUF_SIZE;
    if (next != ktail) {          /* drop if full */
        kbuf[khead] = c;
        khead = next;
        /* wake a thread blocked in keyboard_getchar_blocking(). thread_unblock
           only flips state to READY - it does NOT context switch - so it is
           safe to call from an interrupt handler. */
        if (kwaiter >= 0) thread_unblock(kwaiter);
    }
    spin_unlock(&kbd_lock, f);
}

/* While the display server holds the input handle, a key goes to it and
   never reaches the buffer anybody with GRANT_INPUT could read (2.4). */
void keyboard_inject(unsigned char c) {
    if (!input_post_key(c)) kbuf_push(c);
}

/* Under the lock, as a reader's choice of how long to sleep is: it either saw
   the device, or is prepared to block by the time this wakes it. A USB key
   cannot wake it: nothing fetches USB reports but a reader. */
void keyboard_usb_arrived(void) {
    uint32_t f = spin_lock(&kbd_lock);
    if (kwaiter >= 0) thread_unblock(kwaiter);
    spin_unlock(&kbd_lock, f);
}

uint32_t keyboard_test_hold(void)          { return spin_lock(&kbd_lock); }
void     keyboard_test_release(uint32_t f) { spin_unlock(&kbd_lock, f); }

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

    uint32_t f = spin_lock(&kbd_lock);
    char c = 0;
    if (ktail != khead) {                  /* not empty */
        c = (char)kbuf[ktail];
        ktail = (ktail + 1) % KBD_BUF_SIZE;
    }
    spin_unlock(&kbd_lock, f);
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

        /* Descheduled until a key arrives and wakes us, with the buffer's
           lock held from the look at it to the block's preparation, so a key
           landing between them, on any CPU, is not slept through. With a
           USB HID device it also wakes to poll: the transfer runs here, in
           this thread, never in an interrupt handler. */
        uint32_t f = spin_lock(&kbd_lock);
        kwaiter = thread_current_id();
        int empty = (ktail == khead);
        if (empty) thread_block_prepare(usb_hid_present() ? KBD_USB_POLL_MS : 0);
        spin_unlock(&kbd_lock, f);
        if (empty) thread_block_finish();
        /* Only its own: a reader on another CPU may have named itself since. */
        f = spin_lock(&kbd_lock);
        if (kwaiter == thread_current_id()) kwaiter = -1;
        spin_unlock(&kbd_lock, f);
        if (thread_killed()) return 0;   /* no key: the thread ends on its way out */
    }
}

