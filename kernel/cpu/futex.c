// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/futex.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Futexes (SYS_FUTEX_WAIT, SYS_FUTEX_WAKE; roadmap 1.5b2): a wait on a word
 * of user memory, which ring 3 builds its mutexes, once and condition
 * variables on (std/sync.xfxn). The kernel holds no lock state, only who is
 * waiting on which word; what the word means is the library's.
 *
 * KEYS. A word is named by what it is a part of, not by its address, so two
 * mappings of one memory object - in one process or two - name the same word
 * (Serenity get_futex_key, XNU ULK_XPROC): a word in one of the process's
 * regions is (its memory object, the byte offset in it), and the waiter holds
 * a reference to the object while it waits, so the key cannot be reused under
 * it. A word in no region (the image's data, the first thread's stack) is
 * (the pid, the address), private to the process (XNU ULK_UADDR).
 *
 * THE CHECK AND THE BLOCK are one step under futex_lock, kobj_wait's pattern:
 * a waiter reads the word and queues itself under the lock, and a waker takes
 * the lock to find it, so a wake between the check and the block finds the
 * waiter already queued and BLOCKED (thread_block_prepare_until). The word is
 * faulted in before the lock (user_ptr_readable) and read under it with the
 * recoverable copy, so a word unmapped in between is E_BAD_ADDRESS.
 *
 * A waker takes the waiters it wakes off the queue and marks them, so its
 * count is exact and a waiter whose time ran out at the same moment still
 * returns E_OK: it was woken, and a lock's wake is not lost on it.
 *
 * Lock order: futex_lock, then (a user copy's demand fault) a process's
 * vm lock and its object's; thread_unblock's under it.
 */
#include "futex.h"
#include "sched.h"
#include "usermode.h"          /* user_ptr_readable, user_copy_bytes */
#include "vmregion.h"          /* vm_futex_key */
#include "object/kmem.h"
#include "sync/spinlock.h"
#include "drivers/char/timer.h"
#include "diag/testhook.h"
#include "cxk_abi.h"

struct futex_key {
    struct kmem *mem;      /* the object holding the word, with a reference; or 0, */
    int          pid;      /* and then the word is this process's own */
    uint64_t     at;       /* its offset in mem, or its address in pid */
};

struct futex_waiter {
    struct futex_waiter *next;
    struct futex_key     key;
    int                  tid;
    int                  woken;   /* taken off the queue by a waker */
};

#define FUTEX_BUCKETS 64u

static spinlock_t futex_lock = SPINLOCK_INIT;
static struct futex_waiter *buckets[FUTEX_BUCKETS];   /* each oldest first */
static uint32_t waiting;

static void key_of(uint64_t uaddr, struct futex_key *k) {
    int pid = thread_current_pid();
    if (vm_futex_key(pid, uaddr, &k->mem, &k->at)) { k->pid = -1; return; }
    k->mem = 0;
    k->pid = pid;
    k->at  = uaddr;
}

static void key_drop(struct futex_key *k) { if (k->mem) kobj_unref(&k->mem->hdr); }

static int key_eq(const struct futex_key *a, const struct futex_key *b) {
    return a->mem == b->mem && a->pid == b->pid && a->at == b->at;
}

static struct futex_waiter **bucket(const struct futex_key *k) {
    uint64_t h = (k->at >> 2) ^ (uint64_t)(uintptr_t)k->mem ^ (uint64_t)(uint32_t)k->pid;
    h ^= h >> 17;
    return &buckets[h % FUTEX_BUCKETS];
}

static int word_ok(uint64_t uaddr) {
    if (uaddr & 3u) return E_INVALID_ARGS;
    if (!user_ptr_readable(uaddr, sizeof(uint32_t))) return E_BAD_ADDRESS;
    return E_OK;
}

int futex_wait(uint64_t uaddr, uint32_t expected, uint64_t deadline) {
    int rc = word_ok(uaddr);
    if (rc != E_OK) return rc;
    struct futex_waiter me = { .next = 0, .tid = thread_current_id(), .woken = 0 };
    key_of(uaddr, &me.key);
    if (deadline > TIMER_NEVER) deadline = TIMER_NEVER;
    race_point(RP_FUTEX_READ);

    uint32_t f = spin_lock(&futex_lock);
    uint32_t v;
    if (user_copy_bytes(&v, user_ptr(uaddr), sizeof v) != 0) rc = E_BAD_ADDRESS;
    else if (v != expected)                                     rc = E_SHOULD_WAIT;
    else {
        struct futex_waiter **pp = bucket(&me.key);
        while (*pp) pp = &(*pp)->next;
        *pp = &me;
        waiting++;
        for (;;) {
            /* The block ends at the deadline itself, so the time is up the
               first time this looks after it (kobj_wait). */
            if (deadline < TIMER_NEVER && timer_now_ns() >= deadline) { rc = E_TIMED_OUT; break; }
            thread_block_prepare_until(deadline);
            spin_unlock(&futex_lock, f);
            thread_block_finish();
            race_point(RP_FUTEX_WOKEN);
            f = spin_lock(&futex_lock);
            if (me.woken) break;
            if (thread_killed()) { rc = E_CANCELED; break; }
        }
        if (me.woken) rc = E_OK;
        else {
            pp = bucket(&me.key);
            while (*pp != &me) pp = &(*pp)->next;
            *pp = me.next;
            waiting--;
        }
    }
    spin_unlock(&futex_lock, f);
    key_drop(&me.key);
    return rc;
}

int futex_wake(uint64_t uaddr, uint32_t count) {
    int rc = word_ok(uaddr);
    if (rc != E_OK) return rc;
    struct futex_key k;
    key_of(uaddr, &k);
    uint32_t n = 0;
    uint32_t f = spin_lock(&futex_lock);
    struct futex_waiter **pp = bucket(&k);
    while (*pp && n < count) {
        struct futex_waiter *w = *pp;
        if (!key_eq(&w->key, &k)) { pp = &w->next; continue; }
        *pp = w->next;
        waiting--;
        w->woken = 1;
        thread_unblock(w->tid);
        n++;
    }
    spin_unlock(&futex_lock, f);
    key_drop(&k);
    return (int)n;
}

uint32_t futex_waiting(void) {
    uint32_t f = spin_lock(&futex_lock);
    uint32_t n = waiting;
    spin_unlock(&futex_lock, f);
    return n;
}
