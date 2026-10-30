// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/knotify.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Notifications (KOBJ_NOTIFY; docs/kernel/CX_KERNEL_PLAN.md section 6:
 * "one-bit signals that combine, delivered through the wait queue").
 *
 * A notification is a 64-bit word. Signalling it ORs bits in, so any number
 * of signals before a take combine into one, and nothing blocks the
 * signaller or can fill up (seL4's notification objects; Fuchsia's user
 * signals). SIGNAL_NOTIFY_PENDING is set while any bit is, so a thread waits
 * for it as for any other signal: object_wait, or a queue registration. A
 * take hands the word back and clears it and the signal, at once.
 *
 * Signallers (KOBJ_SIGNALLER) are what the owner gives away: each is minted
 * with a fixed set of bits and can set only those, so a server that gives
 * every client its own bit knows which of them signalled (seL4's badged
 * notification capabilities). A signaller holds a reference, not a handle,
 * on its notification: the notification's own last handle closing is what
 * ends it, and every signal after that is E_PEER_CLOSED.
 *
 * The word and the signal change together under the wait lock (kwait.h), so
 * a waiter that saw the signal always finds the bits it stands for.
 */
#ifndef CXK_KNOTIFY_H
#define CXK_KNOTIFY_H

#include <stdint.h>
#include "object/kobj.h"

struct knotify {
    struct kobj hdr;      /* first; KOBJ_NOTIFY */
    uint64_t    word;     /* bits signalled and not yet taken; under the wait lock */
    uint8_t     closed;   /* the last handle has closed: signals are refused */
};

struct ksignaller {
    struct kobj     hdr;    /* first; KOBJ_SIGNALLER */
    struct knotify *n;      /* a reference */
    uint64_t        bits;   /* the only bits it may set; never 0 */
};

void knotify_sys_init(void);

/* A new notification with no bits set, with one reference for the caller
   (who installs a handle and drops it). 0 when out of memory. */
struct knotify *knotify_create(void);

/* A signaller for `n` that may set `bits` (not 0), with one reference for the
   caller. 0 when out of memory. */
struct ksignaller *ksignaller_create(struct knotify *n, uint64_t bits);

/* OR `bits` into the word and set SIGNAL_NOTIFY_PENDING. E_OK, or
   E_PEER_CLOSED once the notification's last handle has closed. */
int knotify_signal(struct knotify *n, uint64_t bits);

/* The word, cleared, with SIGNAL_NOTIFY_PENDING: 0 if nothing was set. */
uint64_t knotify_take(struct knotify *n);

#endif
