// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/inputdev.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Input as a handle (roadmap 2.4). The rule it exists for: while the display
 * server holds it, no other program can read a key, whatever it was granted.
 * So the events are taken at the point keyboard_inject and mouse_inject
 * receive them, before the buffer the old calls read, and those calls refuse.
 *
 * One object at a time, and it cannot travel (kobj_ops.transfer 0): a handle
 * that could be passed on would be a way to hand somebody the keys. It goes
 * back with its last reference, so a display server that dies gives the
 * keyboard back to the shell.
 *
 * The ring and the signal share the wait lock, as an interrupt object's count
 * does (drivers/devhost.c): events are posted from the keyboard's interrupt.
 */

#include <stdint.h>
#include "inputdev.h"
#include "usb_hid.h"
#include "cxk_abi.h"
#include "object/kobj.h"
#include "object/kwait.h"
#include "object/htab.h"
#include "memman/heap.h"
#include "string.h"
#include "sched.h"
#include "usermode.h"
#include "panic.h"

_Static_assert(KOBJ_INPUT == OBJ_TYPE_INPUT, "object_basic.kind is the kernel type");
_Static_assert(KOBJ_LEASE == OBJ_TYPE_LEASE, "object_basic.kind is the kernel type");

#define INPUT_RING 128u

struct kinput {
    struct kobj hdr;     /* first; KOBJ_INPUT */
    uint32_t head;       /* the oldest; under the wait lock */
    uint32_t count;
    struct input_event ring[INPUT_RING];
};

/* The one holder, or 0; under the wait lock. */
static struct kinput *holder;

static void input_destroy(struct kobj *o) {
    uint32_t f = kwait_lock();
    if (holder == (struct kinput *)o) holder = 0;
    kwait_unlock(f);
    kfree(o);
}

static const struct kobj_ops input_ops = {
    .name          = "input",
    .rights_valid  = HRIGHT_READ,
    .signals_valid = SIGNAL_INPUT_READY,
    .transfer      = 0,
    .destroy       = input_destroy,
};

void input_sys_init(void) {
    if (kobj_type_register(KOBJ_INPUT, &input_ops) != 0)
        kpanic("input_sys_init: the type is taken");
}

/* Under the wait lock. A full ring drops the newest, as the key buffer does. */
static void ring_push(struct kinput *k, const struct input_event *e) {
    if (k->count == INPUT_RING) return;
    k->ring[(k->head + k->count) % INPUT_RING] = *e;
    k->count++;
    kobj_signal_locked(&k->hdr, SIGNAL_INPUT_READY);
}

int input_post_key(unsigned char c) {
    uint32_t f = kwait_lock();
    struct kinput *k = holder;
    if (k) {
        struct input_event e = { 0 };
        e.kind = INPUT_EVENT_KEY;
        e.code = c;
        ring_push(k, &e);
    }
    kwait_unlock(f);
    return k != 0;
}

void input_post_pointer(int32_t x, int32_t y, uint32_t buttons) {
    uint32_t f = kwait_lock();
    struct kinput *k = holder;
    if (k) {
        struct input_event e = { 0 };
        e.kind = INPUT_EVENT_POINTER;
        e.x = x;
        e.y = y;
        e.buttons = buttons;
        struct input_event *last = k->count
            ? &k->ring[(k->head + k->count - 1) % INPUT_RING] : 0;
        if (last && last->kind == INPUT_EVENT_POINTER && last->buttons == buttons) *last = e;
        else ring_push(k, &e);
    }
    kwait_unlock(f);
}

int input_held(void) {
    uint32_t f = kwait_lock();
    int held = holder != 0;
    kwait_unlock(f);
    return held;
}

int sys_input_open(void) {
    struct kinput *k = kmalloc(sizeof *k);
    if (!k) return E_NO_MEMORY;
    memset(k, 0, sizeof *k);
    if (kobj_init(&k->hdr, KOBJ_INPUT) != 0) { kfree(k); return E_NO_MEMORY; }
    uint32_t f = kwait_lock();
    int busy = holder != 0;
    if (!busy) holder = k;
    kwait_unlock(f);
    if (busy) { kobj_unref(&k->hdr); return E_BUSY; }
    int32_t h = htab_install(thread_htab(thread_current_id()), &k->hdr, HRIGHT_READ | HRIGHT_WAIT, 0);
    kobj_unref(&k->hdr);   /* the handle's reference keeps it; with none, it goes back */
    return h;
}

int sys_input_events(int32_t h, uint64_t ev, uint32_t count) {
    if (!count) return E_INVALID_ARGS;
    if (count > INPUT_EVENTS_MAX) count = INPUT_EVENTS_MAX;
    if (!user_ptr_writable(ev, count * sizeof(struct input_event))) return E_BAD_ADDRESS;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_INPUT       ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_READ)  ? E_ACCESS_DENIED : E_OK;
    if (rc != E_OK) { kobj_unref(o); return rc; }

    /* A USB keyboard's reports are fetched by whoever asks for input; these
       transfers sleep, so never in an interrupt and never under the lock. */
    usb_hid_poll();

    struct kinput *k = (struct kinput *)o;
    struct input_event got[INPUT_EVENTS_MAX];
    uint32_t n = 0;
    uint32_t f = kwait_lock();
    while (n < count && k->count) {
        got[n++] = k->ring[k->head];
        k->head = (k->head + 1) % INPUT_RING;
        k->count--;
    }
    if (!k->count) kobj_signal_clear_locked(o, SIGNAL_INPUT_READY);
    kwait_unlock(f);
    kobj_unref(o);
    if (n && user_copy_out(ev, got, n * sizeof got[0]) != 0) return E_BAD_ADDRESS;
    return (int)n;
}
