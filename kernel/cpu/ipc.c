// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/ipc.c */
/* Aurora Tejeda / CATX Systems */
/* IPC: endpoints, badged send handles, queued calls with reply tokens. See ipc.h. */

#include "ipc.h"
#include "object/htab.h"
#include "object/kwait.h"
#include "sync/spinlock.h"
#include "sched.h"
#include "proc.h"
#include "job.h"
#include "caps.h"
#include "usermode.h"   /* user_ptr_readable / user_ptr_writable */
#include "memman/ocache.h"
#include "memman/heap.h"
#include "panic.h"
#include <stddef.h>

_Static_assert(KOBJ_SEND == OBJ_TYPE_SEND && KOBJ_REPLY == OBJ_TYPE_REPLY,
               "object_basic.kind is the kernel type");

/* Endpoints come from their own cache (memman/ocache.h): a stale pointer to
   one can only ever alias another endpoint. The global cap stays as policy. */
#define MAX_ENDPOINTS 32
_Static_assert(sizeof(struct endpoint) <= OCACHE_SPAN, "an endpoint must fit a slab");
static struct ocache ep_cache, send_cache, call_cache;

/* Every endpoint's queue, every call's state. Taken before the wait lock. */
static spinlock_t ipc_lock = SPINLOCK_INIT;

int ep_in_use_count(void) { return (int)ocache_live(&ep_cache); }

int ep_is_cached(const struct endpoint *ep) { return ocache_owns(&ep_cache, ep); }

uint32_t ep_queued(const struct endpoint *ep) { return ep->queued; }

/* ipc_lock held. The endpoint's signal follows what a receive would find. */
static void ep_readable_update(struct endpoint *ep) {
    if (ep->head || ep->stop_pending) kobj_signal(&ep->hdr, SIGNAL_EP_READABLE);
    else                              kobj_signal_clear(&ep->hdr, SIGNAL_EP_READABLE);
}

/* ipc_lock held. Take `c` off its endpoint's queue. */
static void ep_unlink(struct endpoint *ep, struct ipc_call *c) {
    struct ipc_call *prev = 0;
    for (struct ipc_call *q = ep->head; q; prev = q, q = q->next) {
        if (q != c) continue;
        if (prev) prev->next = c->next; else ep->head = c->next;
        if (ep->tail == c) ep->tail = prev;
        ep->queued--;
        break;
    }
    c->next = 0;
    c->ep   = 0;
}

/* ---- Priority through calls (1.3f) ----
 *
 * A thread serving a call runs at least at its caller's priority, so a
 * caller is not held up behind threads that rank between it and the server
 * (XNU's turnstiles, osfmk/kern/turnstile.c; seL4 MCS lends the caller's
 * scheduling context instead, from memory). A thread runs at the highest of
 *
 *   its own level (thread_base);
 *   for each call it took and still answers for, the caller's level;
 *   and that of each call still queued on the endpoint that call came from,
 *
 * the last so that a busy server is lifted by the calls waiting behind it,
 * as XNU's send turnstile pushes on the port's receiver
 * (ipc_port_send_update_inheritor, osfmk/ipc/ipc_port.c). A thread waiting
 * in a call passes on what it runs at: to the thread that took the call, or
 * while it is queued to the threads answering that endpoint. prio_update
 * follows the chain until a level stops changing (XNU
 * turnstile_update_inheritor_priority_chain), PRIO_CHAIN_MAX deep at most.
 * Threads that call each other in a ring keep each other lifted; they are
 * deadlocked anyway.
 *
 * The thread that took a call answers for it: a token passed to another
 * thread or process leaves the lift where it was. A reply, a token closed,
 * the caller killed, or the server thread ending take it off.
 *
 * All under ipc_lock. `serving` lists the calls with a server; there are
 * few, so finding a thread's is a walk (a list per thread can come with SMP).
 */
#define PRIO_CHAIN_MAX 16

static struct ipc_call *serving;

static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

/* The highest level among the callers queued on `ep`. */
static uint32_t ep_queue_prio(const struct endpoint *ep) {
    uint32_t p = 0;
    for (const struct ipc_call *q = ep->head; q; q = q->next)
        if (!q->caller_gone) p = max_u32(p, thread_prio(q->caller_tid));
    return p;
}

static void ep_prio_changed(const struct endpoint *ep, int depth);

/* Run `tid` at what its calls ask for, and pass a change on. */
static void prio_update(int tid, int depth) {
    if (tid < 0 || depth >= PRIO_CHAIN_MAX || !thread_base(tid)) return;
    uint32_t want = thread_base(tid);
    for (const struct ipc_call *c = serving; c; c = c->serve_next) {
        if (c->server_tid != tid) continue;
        if (!c->caller_gone) want = max_u32(want, thread_prio(c->caller_tid));
        if (c->from) want = max_u32(want, ep_queue_prio(c->from));
    }
    if (want == thread_prio(tid)) return;
    thread_set_prio(tid, want);
    const struct ipc_call *in = thread_in_call(tid);
    if (!in) return;
    if (in->server_tid >= 0) prio_update(in->server_tid, depth + 1);
    else if (in->ep)         ep_prio_changed(in->ep, depth + 1);
}

/* `ep`'s queue changed: every thread answering one of its calls. */
static void ep_prio_changed(const struct endpoint *ep, int depth) {
    for (const struct ipc_call *c = serving; c; c = c->serve_next)
        if (c->from == ep) prio_update(c->server_tid, depth);
}

/* The current thread took `c` from `ep`, and answers for it. */
static void call_serve(struct ipc_call *c, struct endpoint *ep) {
    c->server_tid = thread_current_id();
    c->from       = ep;
    c->serve_next = serving;
    serving       = c;
    ep_prio_changed(ep, 0);   /* its server gains the caller; the others lose it */
}

/* `c` no longer lifts its server: answered, closed, or put back. */
static void call_unserve(struct ipc_call *c) {
    if (c->server_tid < 0) return;
    for (struct ipc_call **pp = &serving; *pp; pp = &(*pp)->serve_next)
        if (*pp == c) { *pp = c->serve_next; break; }
    int tid = c->server_tid;
    c->server_tid = -1;
    c->from       = 0;
    c->serve_next = 0;
    prio_update(tid, 0);
}

void ipc_set_base(int tid, uint32_t level) {
    uint32_t f = spin_lock(&ipc_lock);
    thread_set_base(tid, level);
    prio_update(tid, 0);
    spin_unlock(&ipc_lock, f);
}

uint32_t ipc_serving_count(void) {
    uint32_t n = 0;
    uint32_t f = spin_lock(&ipc_lock);
    for (const struct ipc_call *c = serving; c; c = c->serve_next) n++;
    spin_unlock(&ipc_lock, f);
    return n;
}

void ipc_thread_exit(int tid) {
    uint32_t f = spin_lock(&ipc_lock);
    for (struct ipc_call **pp = &serving; *pp; ) {
        struct ipc_call *c = *pp;
        if (c->server_tid != tid) { pp = &c->serve_next; continue; }
        *pp = c->serve_next;
        c->server_tid = -1;
        c->from       = 0;
        c->serve_next = 0;
    }
    spin_unlock(&ipc_lock, f);
}

/* ipc_lock held. End `c` with `result` and wake its caller, if it is still
   there to read it. */
static void call_finish(struct ipc_call *c, int32_t result) {
    c->state  = IPC_DONE;
    c->result = result;
    call_unserve(c);
    if (!c->caller_gone) thread_unblock(c->caller_tid);
}

/* The last receive handle has gone: the server has. Every queued call fails,
   and the receivers standing on it are woken to find it closed. The slot
   survives until the send handles and those threads drop their references. */
static void ep_last_handle(struct kobj *o) {
    struct endpoint *ep = (struct endpoint *)o;
    uint32_t f = spin_lock(&ipc_lock);
    if (ep->active) {
        ep->active = 0;
        while (ep->head) {
            struct ipc_call *c = ep->head;
            ep_unlink(ep, c);
            call_finish(c, E_PEER_CLOSED);
        }
        /* Calls taken from it stay with their servers, but its queue is
           gone, and it may be freed before they are answered. */
        for (struct ipc_call *c = serving; c; c = c->serve_next)
            if (c->from == ep) { c->from = 0; prio_update(c->server_tid, 0); }
        ep->stop_pending = 0;
        /* Set, not cleared: a receive on it returns at once, with an error. */
        kobj_signal(&ep->hdr, SIGNAL_EP_READABLE);
    }
    spin_unlock(&ipc_lock, f);
}

/* Last reference: the endpoint goes back to its cache. Nothing can be queued
   on it: a queued call's caller holds a reference through its send handle. */
static void ep_free(struct kobj *o) {
    ocache_free(&ep_cache, o);
}

static void send_free(struct kobj *o) {
    struct ep_send *s = (struct ep_send *)o;
    ep_put(s->ep);
    ocache_free(&send_cache, s);
}

/* The token's last handle: closed unanswered, or its server's table torn
   down. A call still taken fails; one being answered is the replier's. */
static void call_last_handle(struct kobj *o) {
    struct ipc_call *c = (struct ipc_call *)o;
    uint32_t f = spin_lock(&ipc_lock);
    if (c->state == IPC_TAKEN) call_finish(c, E_PEER_CLOSED);
    spin_unlock(&ipc_lock, f);
}

/* Last reference, held by the caller or the token: the bytes go, and so does
   their charge. The buffer is cleared first: the next owner of that memory is
   another process. */
static void call_free(struct kobj *o) {
    struct ipc_call *c = (struct ipc_call *)o;
    htab_transit_drop(c->h, c->nh);   /* a message nobody will read */
    if (c->buf) {
        for (uint32_t i = 0; i < c->size; i++) c->buf[i] = 0;
        kfree(c->buf);
    }
    if (c->job) {
        job_uncharge_mem(c->job, c->size);
        job_put(c->job);
    }
    ocache_free(&call_cache, c);
}

static const struct kobj_ops ep_ops = {
    .name          = "endpoint",
    .rights_valid  = HRIGHT_RECV,
    .signals_valid = SIGNAL_EP_READABLE,
    .transfer      = 1,
    .last_handle   = ep_last_handle,
    .destroy       = ep_free,
};

static const struct kobj_ops send_ops = {
    .name         = "send",
    .rights_valid = HRIGHT_SEND,
    .transfer     = 1,
    .destroy      = send_free,
};

static const struct kobj_ops call_ops = {
    .name         = "reply",
    .rights_valid = HRIGHT_REPLY,
    .transfer     = 1,
    .last_handle  = call_last_handle,
    .destroy      = call_free,
};

void ipc_init(void) {
    if (ocache_init(&ep_cache, "endpoint", sizeof(struct endpoint)) != 0
        || ocache_init(&send_cache, "send", sizeof(struct ep_send)) != 0
        || ocache_init(&call_cache, "reply", sizeof(struct ipc_call)) != 0)
        kpanic("ipc_init: no endpoint cache");
    kobj_type_register(KOBJ_ENDPOINT, &ep_ops);
    kobj_type_register(KOBJ_SEND, &send_ops);
    kobj_type_register(KOBJ_REPLY, &call_ops);
}

void ep_put(struct endpoint *ep) {
    if (ep) kobj_unref(&ep->hdr);
}

/* Endpoints a process owns, for its quota. */
struct owned { int pid; int n; };
static int count_owned(void *obj, void *ctx) {
    struct owned *o = ctx;
    if (((struct endpoint *)obj)->owner_pid == o->pid) o->n++;
    return 0;
}

int ep_create(void) {
    int me = thread_current_pid();   /* the quota is the process's */

    struct owned o = { me, 0 };
    ocache_walk(&ep_cache, count_owned, &o);
    if (o.n >= MAX_ENDPOINTS_PER_PROC) return E_NO_RESOURCES;
    if (ocache_live(&ep_cache) >= MAX_ENDPOINTS) return E_NO_RESOURCES;

    struct endpoint *ep = ocache_alloc(&ep_cache);   /* zeroed: nothing queued */
    if (!ep) return E_NO_MEMORY;
    if (kobj_init(&ep->hdr, KOBJ_ENDPOINT) != 0) {               /* ipc_init not run */
        ocache_free(&ep_cache, ep);
        return E_INTERNAL;
    }
    ep->active    = 1;
    ep->owner_pid = me;

    int32_t h = htab_install(thread_htab(thread_current_id()), &ep->hdr,
                             HRIGHT_RECV | HRIGHT_WAIT | HRIGHT_DUPLICATE, 0);
    /* The creator's reference goes either way: on success the handle holds
       the endpoint; on failure this was the only reference, and it goes back
       to its cache here. */
    kobj_unref(&ep->hdr);
    return h;   /* htab_install answers in the ABI's codes */
}

int32_t ep_install_handle(int tid, struct endpoint *ep, uint32_t rights, uint32_t parent_grant) {
    if (!ep || !ep->active) return E_BAD_HANDLE;
    return htab_install(thread_htab(tid), &ep->hdr, rights, parent_grant);
}

/* A send handle is a handle on its own object, which holds the endpoint: so
   send handles keep the slot, and only receive handles keep it open. */
int32_t ep_install_send(int tid, struct endpoint *ep, uint64_t badge, uint32_t rights, uint32_t parent_grant) {
    if (!ep || !ep->active) return E_BAD_HANDLE;
    if (!(rights & HRIGHT_SEND) || (rights & ~(HRIGHT_SEND | HRIGHT_DUPLICATE)))
        return E_ACCESS_DENIED;
    struct ep_send *s = ocache_alloc(&send_cache);
    if (!s) return E_NO_MEMORY;
    if (kobj_init(&s->hdr, KOBJ_SEND) != 0) { ocache_free(&send_cache, s); return E_INTERNAL; }
    kobj_ref(&ep->hdr);
    s->ep    = ep;
    s->badge = badge;
    int32_t h = htab_install(thread_htab(tid), &s->hdr, rights, parent_grant);
    kobj_unref(&s->hdr);   /* as in ep_create: the handle keeps it, or it goes */
    return h;
}

struct endpoint *ep_from_handle(int32_t h, uint32_t need_rights, uint32_t *grant) {
    struct htab_info info;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_ENDPOINT,
                              need_rights, &info);
    if (!o) return NULL;

    struct endpoint *ep = (struct endpoint *)o;
    /* A handle on a closed endpoint is a dead name: it resolves to nothing.
       (A receive handle keeps its endpoint open, so this is the case of a
       lookup racing the last close elsewhere - kept as the cheap check.) */
    if (!ep->active) { ep_put(ep); return NULL; }
    if (grant) *grant = info.grant;
    return ep;
}

int ep_mint(int32_t h, uint64_t badge, uint32_t rights) {
    uint32_t grant = 0;
    struct endpoint *ep = ep_from_handle(h, HRIGHT_RECV, &grant);
    if (!ep) {
        /* Tell the refusals apart, as htab_ref would. */
        struct htab_info info;
        struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &info);
        if (!o) return E_BAD_HANDLE;
        int rc = o->type != KOBJ_ENDPOINT ? E_WRONG_TYPE
               : !(info.rights & HRIGHT_RECV) ? E_ACCESS_DENIED : E_BAD_HANDLE;
        kobj_unref(o);
        return rc;
    }
    int32_t s = ep_install_send(thread_current_id(), ep, badge, rights, grant);
    ep_put(ep);
    return s;
}

/* A call for `req_len` bytes taking a reply of up to `rcap`, charged to the
   caller's job. NULL with *rc set if the charge or an allocation fails. */
static struct ipc_call *call_new(uint32_t req_len, uint32_t rcap, int *rc) {
    struct ipc_call *c = ocache_alloc(&call_cache);
    if (!c) { *rc = E_NO_MEMORY; return 0; }
    if (kobj_init(&c->hdr, KOBJ_REPLY) != 0) { ocache_free(&call_cache, c); *rc = E_INTERNAL; return 0; }
    c->size = req_len > rcap ? req_len : rcap;
    c->req_len = req_len;
    c->reply_cap = rcap;
    c->caller_tid = thread_current_id();
    c->caller_pid = thread_current_pid();
    c->server_tid = -1;
    if (c->size) {
        struct proc *p = thread_proc(c->caller_tid);
        struct job *j = p ? p->job : job_root();
        *rc = job_charge_mem(j, c->size);
        if (*rc != E_OK) { c->size = 0; kobj_unref(&c->hdr); return 0; }
        kobj_ref(&j->hdr);
        c->job = j;
        c->buf = kmalloc(c->size);
        if (!c->buf) { kobj_unref(&c->hdr); *rc = E_NO_MEMORY; return 0; }
    }
    return c;
}

/* Client side: queue the call, wake a receiver, block until it is done, then
   copy the reply out. Runs in the caller's address space (its CR3 is live).
   The caller's references, on its send handle (and through it the endpoint)
   and on the call, are held across the block. */
static int call_on(struct ep_send *s, const struct ipc_call_args *ap,
                   const struct htab_send *hs, uint32_t nh) {
    struct ipc_call_args a = *ap;
    struct endpoint *ep = s->ep;
    struct htab *t = thread_htab(thread_current_id());
    if (a.req_len > IPC_MSG_MAX) return E_OUT_OF_RANGE;
    /* No reply is longer than IPC_MSG_MAX, so a larger cap promises room
       nothing will use; only what can be written is checked. The same for
       handles. */
    uint32_t rcap = a.reply_cap > IPC_MSG_MAX ? IPC_MSG_MAX : (uint32_t)a.reply_cap;
    uint32_t rhcap = a.reply_handle_cap > IPC_HANDLES_MAX ? IPC_HANDLES_MAX : a.reply_handle_cap;

    /* An early reject, not the authority: the reply buffer is validated again
       at the copy-out below, which is the check that counts. This one is here
       so a caller that passes an unusable reply buffer is told now rather than
       after a whole round trip through the server. Haiku keeps its up-front
       IS_USER_ADDRESS check for the same reason. */
    if (rcap && !user_ptr_writable(a.reply, rcap)) return E_BAD_ADDRESS;
    if (rhcap && !user_ptr_writable(a.reply_handles, rhcap * sizeof(int32_t))) return E_BAD_ADDRESS;
    if (!ep->active) return E_PEER_CLOSED;

    int rc = E_OK;
    struct ipc_call *c = call_new((uint32_t)a.req_len, rcap, &rc);
    if (!c) return rc;
    if (a.req_len && user_copy_in(c->buf, a.req, (uint32_t)a.req_len) != 0) {
        kobj_unref(&c->hdr);
        return E_BAD_ADDRESS;
    }
    c->badge = s->badge;
    c->reply_hcap = rhcap;

    uint32_t f = spin_lock(&ipc_lock);
    if (!ep->active) {                 /* the server went while this was copied */
        spin_unlock(&ipc_lock, f);
        kobj_unref(&c->hdr);
        return E_PEER_CLOSED;
    }
    /* The handles go as the call is queued, under the same lock, so a call
       refused leaves them all where they were. */
    if (nh) {
        rc = htab_take(t, hs, nh, &ep->hdr, c->h);
        if (rc != 0) {
            spin_unlock(&ipc_lock, f);
            kobj_unref(&c->hdr);
            return rc;
        }
        c->nh = nh;
    }
    c->ep = ep;
    if (ep->tail) ep->tail->next = c; else ep->head = c;
    ep->tail = c;
    ep->queued++;
    ep_readable_update(ep);
    /* The caller's level reaches whoever answers the endpoint, and goes on
       reaching them while it waits (prio_update follows in_call). */
    thread_set_in_call(thread_current_id(), c);
    ep_prio_changed(ep, 0);

    /* BLOCKED before the lock goes, so a reply from here on is not lost. */
    while (c->state != IPC_DONE) {
        thread_block_prepare(0);
        spin_unlock(&ipc_lock, f);
        thread_block_finish();
        f = spin_lock(&ipc_lock);
        if (c->state != IPC_DONE && thread_killed()) break;
    }
    thread_set_in_call(thread_current_id(), 0);

    /* Killed. A call nobody has taken is withdrawn; one the server is working
       on stays with its token, and the reply finds the caller gone. One
       already answered was copied out as usual. Either way it lifts nobody
       from here. */
    if (c->state != IPC_DONE) {
        c->caller_gone = 1;
        if (c->state == IPC_QUEUED) {
            ep_unlink(ep, c);
            ep_readable_update(ep);
            c->state  = IPC_DONE;
            c->result = E_CANCELED;
            ep_prio_changed(ep, 0);
        } else {
            prio_update(c->server_tid, 0);
        }
        spin_unlock(&ipc_lock, f);
        kobj_unref(&c->hdr);
        return E_CANCELED;
    }
    spin_unlock(&ipc_lock, f);

    /* DONE is final, and the server's part of buf and h ended with it. A
       reply's handles land after its bytes are out; ones that cannot are
       dropped with the call. */
    rc = c->result;
    if (rc > 0 && user_copy_out(a.reply, c->buf, (uint32_t)rc) != 0) rc = E_BAD_ADDRESS;
    if (rc >= 0 && rhcap) {
        int32_t v[IPC_HANDLES_MAX] = { 0 };
        uint32_t n = c->nh;
        if (n && htab_land(t, c->h, n, v) != 0) {
            rc = E_NO_RESOURCES;
        } else {
            c->nh = 0;
            if (user_copy_out(a.reply_handles, v, rhcap * sizeof v[0]) != 0) {
                for (uint32_t i = 0; i < n; i++) htab_close(t, v[i]);
                rc = E_BAD_ADDRESS;
            }
        }
    }
    kobj_unref(&c->hdr);
    return rc;
}

/* Read `n` ipc_handle entries from user `ua` into `s`. Checked before the
   handle the call names, as the reserved fields are. */
static int handles_in(uint64_t ua, uint32_t n, struct htab_send *s) {
    if (n > IPC_HANDLES_MAX) return E_OUT_OF_RANGE;
    struct ipc_handle d[IPC_HANDLES_MAX];
    if (n && user_copy_in(d, ua, n * (uint32_t)sizeof d[0]) != 0) return E_BAD_ADDRESS;
    for (uint32_t i = 0; i < n; i++) {
        if (d[i].reserved != 0) return E_INVALID_ARGS;
        s[i].h      = d[i].handle;
        s[i].op     = d[i].op;
        s[i].rights = d[i].rights;
    }
    return 0;
}

int ipc_call(uint64_t ua) {
    struct ipc_call_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;
    if (a.reserved != 0) return E_INVALID_ARGS;
    struct htab_send hs[IPC_HANDLES_MAX];
    int rc = handles_in(a.handles, a.handle_count, hs);
    if (rc != 0) return rc;

    struct kobj *o = htab_ref(thread_htab(thread_current_id()), a.ep_handle, KOBJ_SEND,
                              HRIGHT_SEND, 0);
    if (!o) return E_BAD_HANDLE;
    rc = call_on((struct ep_send *)o, &a, hs, a.handle_count);
    kobj_unref(o);
    return rc;
}

/* Server side: wait for a call or a notice, then deliver it. Runs in the
   server's address space. The reference is held as in ipc_call. */
static int recv_on(struct endpoint *ep, const struct ipc_recv_args *ap) {
    struct ipc_recv_args a = *ap;
    struct htab *t = thread_htab(thread_current_id());
    /* As in ipc_call, a cap past the largest message promises unused room. */
    uint32_t cap = a.cap > IPC_MSG_MAX ? IPC_MSG_MAX : (uint32_t)a.cap;
    uint32_t hcap = a.handle_cap > IPC_HANDLES_MAX ? IPC_HANDLES_MAX : a.handle_cap;
    /* Early rejects, as in ipc_call: the buffers are validated again at the
       copies below, which are the checks that count. */
    if (cap && !user_ptr_writable(a.buf, cap)) return E_BAD_ADDRESS;
    if (hcap && !user_ptr_writable(a.handles, hcap * sizeof(int32_t))) return E_BAD_ADDRESS;
    if (!user_ptr_writable(a.info, sizeof(struct ipc_msg_info))) return E_BAD_ADDRESS;

    struct ipc_msg_info info = { 0 };
    uint32_t f = spin_lock(&ipc_lock);
    while (ep->active && !ep->head && !ep->stop_pending) {
        spin_unlock(&ipc_lock, f);
        /* The signal is set under ipc_lock before a call's lock goes, so one
           queued between the look and the wait is not missed. */
        int w = kobj_wait(&ep->hdr, SIGNAL_EP_READABLE, KWAIT_NEVER, 0);
        if (w != E_OK) return E_CANCELED;   /* killed */
        f = spin_lock(&ipc_lock);
    }
    /* A last-handle close wakes us with nothing to take. */
    if (!ep->active) { spin_unlock(&ipc_lock, f); return E_CANCELED; }

    /* A stop notice goes first, or a busy server would never see it. It is
       not a call: nothing is taken, and there is nobody to reply to. It
       stays if it cannot be delivered whole. */
    if (ep->stop_pending) {
        struct stop_notice n = { NOTICE_STOP, ep->stop_by, ep->stop_ms, 0 };
        info.len    = sizeof n;
        info.sender = IPC_SENDER_KERNEL;
        if (cap < sizeof n) {
            spin_unlock(&ipc_lock, f);
            return user_copy_out(a.info, &info, sizeof info) != 0 ? E_BAD_ADDRESS : E_BUFFER_TOO_SMALL;
        }
        ep->stop_pending = 0;
        ep_readable_update(ep);
        spin_unlock(&ipc_lock, f);
        if (user_copy_out(a.buf, &n, sizeof n) != 0
            || user_copy_out(a.info, &info, sizeof info) != 0)
            return E_BAD_ADDRESS;
        return (int)sizeof n;
    }

    struct ipc_call *c = ep->head;
    info.badge     = c->badge;
    info.len       = c->req_len;
    info.reply_cap = c->reply_cap;
    info.sender    = c->caller_pid;
    info.handles   = c->nh;
    info.reply_handles = c->reply_hcap;
    /* One that does not fit stays at the head, for a receive with room. */
    if (c->req_len > cap || c->nh > hcap) {
        spin_unlock(&ipc_lock, f);
        return user_copy_out(a.info, &info, sizeof info) != 0 ? E_BAD_ADDRESS : E_BUFFER_TOO_SMALL;
    }
    ep_unlink(ep, c);
    ep_readable_update(ep);
    c->state = IPC_TAKEN;
    call_serve(c, ep);
    kobj_ref(&c->hdr);   /* until the token is installed: the caller may leave */
    spin_unlock(&ipc_lock, f);

    /* The handles and the token land together. No room for them all: the
       call goes back to the head, as it was, handles and all. */
    struct htab_transit x[IPC_HANDLES_MAX + 1];
    int32_t v[IPC_HANDLES_MAX + 1];
    uint32_t nh = c->nh;
    for (uint32_t i = 0; i < nh; i++) x[i] = c->h[i];
    x[nh] = (struct htab_transit){ &c->hdr, HRIGHT_REPLY, 0, 0 };   /* a new handle */
    int32_t tok = E_NO_RESOURCES;
    if (htab_land(t, x, nh + 1, v) == 0) {
        c->nh = 0;
        tok = v[nh];
    }
    if (tok < 0) {
        f = spin_lock(&ipc_lock);
        call_unserve(c);
        if (c->caller_gone) {
            c->state = IPC_DONE;
        } else if (!ep->active) {
            call_finish(c, E_PEER_CLOSED);
        } else {
            c->state = IPC_QUEUED;
            c->ep    = ep;
            c->next  = ep->head;
            ep->head = c;
            if (!ep->tail) ep->tail = c;
            ep->queued++;
            ep_readable_update(ep);
            ep_prio_changed(ep, 0);
        }
        spin_unlock(&ipc_lock, f);
        kobj_unref(&c->hdr);
        return E_NO_RESOURCES;
    }
    info.reply = tok;

    /* The token's handle holds the call from here, and with it the bytes
       copied below. A copy that fails loses the message, as it does in
       Haiku's _user_read_port_etc: the token is closed, and the caller is
       told E_PEER_CLOSED. The up-front checks are what keep that narrow. */
    int rc = (c->req_len && user_copy_out(a.buf, c->buf, c->req_len) != 0)
          || (nh && user_copy_out(a.handles, v, nh * (uint32_t)sizeof v[0]) != 0)
          || user_copy_out(a.info, &info, sizeof info) != 0 ? E_BAD_ADDRESS : (int)c->req_len;
    kobj_unref(&c->hdr);
    /* The handles are the server's now, but it cannot know their values. */
    if (rc < 0) for (uint32_t i = 0; i <= nh; i++) htab_close(t, v[i]);
    return rc;
}

int ipc_recv(uint64_t ua) {
    struct ipc_recv_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV, NULL);
    if (!ep) return E_BAD_HANDLE;
    int rc = recv_on(ep, &a);
    ep_put(ep);
    return rc;
}

int ep_post_stop(struct endpoint *ep, int by, uint32_t ms) {
    uint32_t f = spin_lock(&ipc_lock);
    if (!ep->active) { spin_unlock(&ipc_lock, f); return E_PEER_CLOSED; }
    ep->stop_by      = (uint32_t)by;
    ep->stop_ms      = ms;
    ep->stop_pending = 1;
    ep_readable_update(ep);
    spin_unlock(&ipc_lock, f);
    return 0;
}

/* Server side: answer the call behind token `tok` and close the token. */
static int reply_on(struct ipc_call *c, int32_t tok, const struct ipc_reply_args *ap,
                    const struct htab_send *hs, uint32_t nh) {
    struct ipc_reply_args a = *ap;
    struct htab *t = thread_htab(thread_current_id());

    uint32_t f = spin_lock(&ipc_lock);
    /* Another thread of the server is answering it. */
    if (c->state != IPC_TAKEN) { spin_unlock(&ipc_lock, f); return E_INVALID_ARGS; }
    /* Killed while this was being worked on: the call ends here. */
    if (c->caller_gone) {
        c->state = IPC_DONE;
        call_unserve(c);
        spin_unlock(&ipc_lock, f);
        htab_close(t, tok);
        return E_PEER_CLOSED;
    }
    if (a.len > c->reply_cap || nh > c->reply_hcap) { spin_unlock(&ipc_lock, f); return E_BUFFER_TOO_SMALL; }
    c->state = IPC_REPLYING;
    spin_unlock(&ipc_lock, f);

    /* Read in the server's space, with buf its own until the state moves on.
       A failure leaves the call taken, for another try or a close. */
    int bad = a.len && user_copy_in(c->buf, a.data, (uint32_t)a.len) != 0;

    f = spin_lock(&ipc_lock);
    int rc = E_OK, keep = 0;
    /* The handles go last, as the call ends, under the lock: a reply refused
       for one of them keeps them all, and its token. */
    if (!bad && !c->caller_gone && nh) {
        rc = htab_take(t, hs, nh, &c->hdr, c->h);
        if (rc == 0) c->nh = nh;
    }
    if (bad || rc != 0) {
        /* Back to taken, unless another thread closed the token meanwhile:
           its last_handle hook passed over a call being answered. */
        c->state = IPC_TAKEN;
        if (atomic_load(&c->hdr.handles) == 0) call_finish(c, E_PEER_CLOSED);
        if (bad) rc = E_BAD_ADDRESS;
        keep = 1;
    } else if (c->caller_gone) {
        c->state = IPC_DONE;
        call_unserve(c);
        rc = E_PEER_CLOSED;
    } else {
        call_finish(c, (int32_t)a.len);   /* the caller resumes in ipc_call, copies it out */
    }
    spin_unlock(&ipc_lock, f);
    if (!keep) htab_close(t, tok);
    return rc;
}

int ipc_reply(uint64_t ua) {
    struct ipc_reply_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;
    struct htab_send hs[IPC_HANDLES_MAX];
    int rc = handles_in(a.handles, a.handle_count, hs);
    if (rc != 0) return rc;

    struct kobj *o = htab_ref(thread_htab(thread_current_id()), a.token, KOBJ_REPLY,
                              HRIGHT_REPLY, 0);
    if (!o) return E_BAD_HANDLE;
    rc = reply_on((struct ipc_call *)o, a.token, &a, hs, a.handle_count);
    kobj_unref(o);
    return rc;
}
