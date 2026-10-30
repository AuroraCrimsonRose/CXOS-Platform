// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kthread.c */
/* Aurora Tejeda / CATX Systems */
/* Thread objects: see kthread.h. */
#include "object/kthread.h"
#include "object/kwait.h"
#include "memman/ocache.h"
#include "cxk_abi.h"
#include "panic.h"

_Static_assert(KOBJ_THREAD == OBJ_TYPE_THREAD, "object_basic.kind is the kernel type");

static struct ocache kthread_cache;

static void kthread_destroy(struct kobj *o) { ocache_free(&kthread_cache, o); }

/* Not transferable yet: a thread's handle means something only in its own
   process until a cross-process use for it exists (a debugger's). */
static const struct kobj_ops kthread_ops = {
    .name          = "thread",
    .rights_valid  = HRIGHT_INSPECT,
    .signals_valid = SIGNAL_THREAD_ENDED,
    .transfer      = 0,
    .destroy       = kthread_destroy,
};

void kthread_sys_init(void) {
    if (ocache_init(&kthread_cache, "thread", sizeof(struct kthread)) != 0 ||
        kobj_type_register(KOBJ_THREAD, &kthread_ops) != 0)
        kpanic("kthread_sys_init: no thread object cache");
}

struct kthread *kthread_create(int tid) {
    struct kthread *k = ocache_alloc(&kthread_cache);   /* zeroed: THR_RUNNING */
    if (!k) return 0;
    if (kobj_init(&k->hdr, KOBJ_THREAD) != 0) { ocache_free(&kthread_cache, k); return 0; }
    k->tid = tid;
    return k;
}

void kthread_end(struct kthread *k, uint32_t how, int32_t code, uint64_t cpu_ns, const char *name) {
    /* Under the wait lock, as the signal is set: a waiter that wakes on it
       reads how it ended, never THR_RUNNING. */
    uint32_t f = kwait_lock();
    int first = k->ended == THR_RUNNING;
    if (first) {
        k->ended  = how;
        k->code   = code;
        k->cpu_ns = cpu_ns;
        uint32_t i = 0;
        for (; name && i + 1u < THREAD_NAME_MAX && name[i]; i++) k->name[i] = name[i];
        k->name[i] = 0;
    }
    kwait_unlock(f);
    if (first) kobj_signal(&k->hdr, SIGNAL_THREAD_ENDED);
}
