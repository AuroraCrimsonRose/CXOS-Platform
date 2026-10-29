// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kobj.c */
/* Aurora Tejeda / CATX Systems */
/* Kernel objects: see kobj.h. */
#include "object/kobj.h"

#ifndef CXK_HOST_TEST
#include "panic.h"
/* A count below zero means memory is already wrong; carrying on would turn
   it into a use-after-free somewhere far from the cause. */
void kobj_panic(const char *why) { kpanic(why); }
#endif

static const struct kobj_ops *types[KOBJ_TYPE_MAX];
static atomic_u32 live[KOBJ_TYPE_MAX];
static atomic_u32 next_koid = { 1 };

int kobj_type_register(enum kobj_type t, const struct kobj_ops *ops) {
    if ((unsigned)t == KOBJ_NONE || (unsigned)t >= KOBJ_TYPE_MAX) return -1;
    if (!ops || !ops->destroy) return -1;
    if (types[t]) return -1;
    types[t] = ops;
    return 0;
}

const struct kobj_ops *kobj_type_ops(enum kobj_type t) {
    if ((unsigned)t >= KOBJ_TYPE_MAX) return 0;
    return types[t];
}

int kobj_init(struct kobj *o, enum kobj_type t) {
    if (!kobj_type_ops(t)) return -1;
    o->type = (uint16_t)t;
    o->_pad = 0;
    atomic_store(&o->refs, 1);
    atomic_store(&o->handles, 0);
    atomic_store(&o->signals, 0);
    o->waiters = 0;
    o->qregs   = 0;
    do o->koid = atomic_fetch_add(&next_koid, 1); while (o->koid == 0);   /* 0: none */
    atomic_fetch_add(&live[t], 1);
    return 0;
}

void kobj_ref(struct kobj *o) {
    /* Taking a reference to an object nobody references is a use after
       free in the making: the caller's pointer is already dangling. */
    if (atomic_fetch_add(&o->refs, 1) == 0)
        kobj_panic("kobj_ref on an object with no references");
}

void kobj_unref(struct kobj *o) {
    uint32_t before = atomic_fetch_sub(&o->refs, 1);
    if (before == 0) {
        atomic_store(&o->refs, 0);
        kobj_panic("kobj_unref below zero");
        return;
    }
    if (before != 1) return;

    /* The last reference. Nothing else can reach the object, so its type and
       hooks are read without racing anyone. */
    enum kobj_type t = (enum kobj_type)o->type;
    atomic_fetch_sub(&live[t], 1);
    types[t]->destroy(o);
}

void kobj_handle_opened(struct kobj *o) {
    kobj_ref(o);
    atomic_fetch_add(&o->handles, 1);
}

void kobj_handle_closed(struct kobj *o) {
    uint32_t before = atomic_fetch_sub(&o->handles, 1);
    if (before == 0) {
        atomic_store(&o->handles, 0);
        kobj_panic("kobj_handle_closed with no handles open");
        return;
    }
    if (before == 1) {
        const struct kobj_ops *ops = types[o->type];
        if (ops->last_handle) ops->last_handle(o);
    }
    kobj_unref(o);
}

uint32_t kobj_live(enum kobj_type t) {
    if ((unsigned)t >= KOBJ_TYPE_MAX) return 0;
    return atomic_load(&live[t]);
}
