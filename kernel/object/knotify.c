// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/knotify.c */
/* Aurora Tejeda / CATX Systems */
/* Notifications and their signallers: see knotify.h. */
#include "object/knotify.h"
#include "object/kwait.h"
#include "memman/ocache.h"
#include "cxk_abi.h"
#include "panic.h"

_Static_assert(KOBJ_NOTIFY == OBJ_TYPE_NOTIFY, "object_basic.kind is the kernel type");
_Static_assert(KOBJ_SIGNALLER == OBJ_TYPE_SIGNALLER, "object_basic.kind is the kernel type");

static struct ocache notify_cache;
static struct ocache signaller_cache;

/* Nobody can take from it any more: a signaller learns so on its next signal.
   A waiter still on it (another thread closed the handle) holds a reference
   and keeps waiting, as on a timer; nothing will set it now. */
static void notify_last_handle(struct kobj *o) {
    uint32_t f = kwait_lock();
    ((struct knotify *)o)->closed = 1;
    kwait_unlock(f);
}

static void notify_destroy(struct kobj *o) { ocache_free(&notify_cache, o); }

static void signaller_destroy(struct kobj *o) {
    struct ksignaller *s = (struct ksignaller *)o;
    kobj_unref(&s->n->hdr);
    ocache_free(&signaller_cache, s);
}

static const struct kobj_ops notify_ops = {
    .name          = "notify",
    .rights_valid  = HRIGHT_SIGNAL | HRIGHT_RECV,
    .signals_valid = SIGNAL_NOTIFY_PENDING,
    .transfer      = 1,
    .last_handle   = notify_last_handle,
    .destroy       = notify_destroy,
};

static const struct kobj_ops signaller_ops = {
    .name         = "signaller",
    .rights_valid = HRIGHT_SIGNAL,
    .transfer     = 1,
    .destroy      = signaller_destroy,
};

void knotify_sys_init(void) {
    if (ocache_init(&notify_cache, "notify", sizeof(struct knotify)) != 0 ||
        ocache_init(&signaller_cache, "signaller", sizeof(struct ksignaller)) != 0 ||
        kobj_type_register(KOBJ_NOTIFY, &notify_ops) != 0 ||
        kobj_type_register(KOBJ_SIGNALLER, &signaller_ops) != 0)
        kpanic("knotify_sys_init: no notification cache");
}

struct knotify *knotify_create(void) {
    struct knotify *n = ocache_alloc(&notify_cache);   /* zeroed: nothing set */
    if (!n) return 0;
    if (kobj_init(&n->hdr, KOBJ_NOTIFY) != 0) { ocache_free(&notify_cache, n); return 0; }
    return n;
}

struct ksignaller *ksignaller_create(struct knotify *n, uint64_t bits) {
    struct ksignaller *s = ocache_alloc(&signaller_cache);
    if (!s) return 0;
    if (kobj_init(&s->hdr, KOBJ_SIGNALLER) != 0) { ocache_free(&signaller_cache, s); return 0; }
    kobj_ref(&n->hdr);
    s->n    = n;
    s->bits = bits;
    return s;
}

int knotify_signal(struct knotify *n, uint64_t bits) {
    int rc = E_PEER_CLOSED;
    uint32_t f = kwait_lock();
    if (!n->closed) {
        n->word |= bits;
        kobj_signal_locked(&n->hdr, SIGNAL_NOTIFY_PENDING);
        rc = E_OK;
    }
    kwait_unlock(f);
    return rc;
}

uint64_t knotify_take(struct knotify *n) {
    uint32_t f = kwait_lock();
    uint64_t w = n->word;
    n->word = 0;
    kobj_signal_clear_locked(&n->hdr, SIGNAL_NOTIFY_PENDING);
    kwait_unlock(f);
    return w;
}
