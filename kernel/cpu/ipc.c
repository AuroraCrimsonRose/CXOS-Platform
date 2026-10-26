// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/ipc.c */
/* Aurora Tejeda / CATX Systems */
/* ABI v1 IPC: endpoints + synchronous call/recv/reply rendezvous. See ipc.h. */

#include "ipc.h"
#include "object/htab.h"
#include "sched.h"
#include "caps.h"
#include "usermode.h"   /* user_ptr_readable / user_ptr_writable */
#include "memman/ocache.h"
#include "panic.h"
#include <stddef.h>

/* Endpoints come from their own cache (memman/ocache.h): a stale pointer to
   one can only ever alias another endpoint. The global cap stays as policy. */
#define MAX_ENDPOINTS 32
_Static_assert(sizeof(struct endpoint) <= OCACHE_SPAN, "an endpoint must fit a slab");
static struct ocache ep_cache;

int ep_in_use_count(void) { return (int)ocache_live(&ep_cache); }

int ep_is_cached(const struct endpoint *ep) { return ocache_owns(&ep_cache, ep); }

/* The last handle has gone. Close the endpoint for business and wake anyone
   standing on it, so a peer that closes mid-rendezvous ends the other side
   with an error instead of a permanent block. The slot itself survives until
   those woken threads drop their own references. */
static void ep_last_handle(struct kobj *o) {
    struct endpoint *ep = (struct endpoint *)o;
    if (!ep->active) return;
    ep->active = 0;

    if (ep->recv_tid != -1) {
        int t = ep->recv_tid;
        ep->recv_tid = -1;
        thread_unblock(t);
    }
    if (ep->caller_tid != -1)
        thread_unblock(ep->caller_tid);
}

/* Last reference: the endpoint goes back to its cache. The cache fills it with
   poison as it does, and that fill is not tidiness: buf has held request and
   reply bodies, and the next owner of this memory is a different process. */
static void ep_free(struct kobj *o) {
    ocache_free(&ep_cache, o);
}

static const struct kobj_ops ep_ops = {
    .name         = "endpoint",
    .rights_valid = HRIGHT_SEND | HRIGHT_RECV,
    .last_handle  = ep_last_handle,
    .destroy      = ep_free,
};

void ipc_init(void) {
    if (ocache_init(&ep_cache, "endpoint", sizeof(struct endpoint)) != 0)
        kpanic("ipc_init: no endpoint cache");
    kobj_type_register(KOBJ_ENDPOINT, &ep_ops);
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

    struct endpoint *ep = ocache_alloc(&ep_cache);
    if (!ep) return E_NO_MEMORY;
    if (kobj_init(&ep->hdr, KOBJ_ENDPOINT) != 0) {               /* ipc_init not run */
        ocache_free(&ep_cache, ep);
        return E_INTERNAL;
    }
    ep->active       = 1;
    ep->owner_pid    = me;
    ep->recv_tid     = -1;
    ep->caller_tid   = -1;
    ep->caller_pid   = -1;
    ep->reply_ptr    = 0;
    ep->reply_cap    = 0;
    ep->msg_len      = 0;

    int32_t h = htab_install(thread_htab(thread_current_id()), &ep->hdr, HRIGHT_RECV | HRIGHT_DUPLICATE, 0);
    /* The creator's reference goes either way: on success the handle holds
       the endpoint; on failure this was the only reference, and it goes back
       to its cache here. */
    kobj_unref(&ep->hdr);
    return h < 0 ? E_NO_RESOURCES : h;
}

/* A further handle onto an existing endpoint - spawn.c hands every child a
   SEND right to the broker's. Each handle is a reference, which is exactly why
   the first close must not free. */
int32_t ep_install_handle(int tid, struct endpoint *ep, uint32_t rights, uint32_t parent_grant) {
    if (!ep || !ep->active) return E_BAD_HANDLE;
    return htab_install(thread_htab(tid), &ep->hdr, rights, parent_grant);
}

struct endpoint *ep_from_handle(int32_t h, uint32_t need_rights, uint32_t *grant) {
    struct htab_info info;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_ENDPOINT,
                              need_rights, &info);
    if (!o) return NULL;

    struct endpoint *ep = (struct endpoint *)o;
    /* A handle on a destroyed endpoint is a dead name: it resolves to nothing.
       (The table refuses a closed handle's number outright; this is the case
       of a live handle whose endpoint lost its last handle elsewhere, which
       cannot happen while this handle exists - kept as the cheap check.) */
    if (!ep->active) { ep_put(ep); return NULL; }
    if (grant) *grant = info.grant;
    return ep;
}

/* app side: stage the request, wake a waiting owner, block until replied, then
   copy the reply out. Runs in the caller's address space (its CR3 is live).
   The caller's reference (ipc_call below) is held across the block: the owner
   may close its handle while we sleep, and without it the slot could be freed
   and reused under us - we would wake and copy a reply out of some other
   process's endpoint. */
static int call_on(struct endpoint *ep, const struct ipc_call_args *ap) {
    struct ipc_call_args a = *ap;
    if (a.req_len > sizeof ep->buf) return E_OUT_OF_RANGE;

    /* An early reject, not the authority: the reply buffer is validated again
       at the copy-out below, which is the check that counts. This one is here
       so a caller that passes an unusable reply buffer is told now rather than
       after a whole round trip through the broker. Haiku keeps its up-front
       IS_USER_ADDRESS check for the same reason. */
    if (a.reply_cap && !user_ptr_writable(a.reply, a.reply_cap)) return E_BAD_ADDRESS;

    /* Before anything writes ep->buf: it still holds an in-flight
       transaction's message if one is live. */
    if (ep->caller_tid != -1) return E_BUSY;   /* single in-flight (v1) */

    if (user_copy_in(ep->buf, a.req, a.req_len) != 0) return E_BAD_ADDRESS;
    ep->msg_len    = a.req_len;
    ep->reply_ptr  = a.reply;
    ep->reply_cap  = a.reply_cap;
    ep->caller_tid = thread_current_id();
    ep->caller_pid = thread_current_pid();   /* what ipc_recv reports */

    if (ep->recv_tid != -1) { int t = ep->recv_tid; ep->recv_tid = -1; thread_unblock(t); }

    thread_block();   /* until ipc_reply wakes us; reply now staged in ep->buf */

    /* The last handle's hook also wakes us, so waking does not mean a reply
       arrived. */
    if (!ep->active) {
        ep->caller_tid = -1;
        return E_PEER_CLOSED;
    }

    /* The copy-out, re-validated against the endpoint's own record of the
       transaction rather than the pointer this frame validated before it
       slept (security review §6). reply_ptr/reply_cap were stored and never
       read before this: the copy used the stack copy, so the kernel kept a
       record of the reply buffer that nothing consulted.

       It is deliberately done while the reference is still held - dropping it
       may return the slot to the pool and wipe buf. */
    uint32_t rl = ep->msg_len;
    if (rl > ep->reply_cap) rl = ep->reply_cap;
    int rc = user_copy_out(ep->reply_ptr, ep->buf, rl);

    ep->caller_tid = -1;   /* rendezvous complete either way */
    return rc != 0 ? E_BAD_ADDRESS : (int)rl;
}

int ipc_call(uint32_t ua) {
    struct ipc_call_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_SEND, NULL);
    if (!ep) return E_BAD_HANDLE;
    int rc = call_on(ep, &a);
    ep_put(ep);
    return rc;
}

/* owner side: wait for a caller, then copy the staged request out. Runs in the
   owner's (executive's) address space. The reference is held as in ipc_call. */
static int recv_on(struct endpoint *ep, const struct ipc_recv_args *ap) {
    struct ipc_recv_args a = *ap;
    /* Early rejects, as in ipc_call: both buffers are validated again at the
       copies below, which are the checks that count. */
    if (a.cap && !user_ptr_writable(a.buf, a.cap)) return E_BAD_ADDRESS;
    if (a.sender && !user_ptr_writable(a.sender, sizeof(int))) return E_BAD_ADDRESS;

    while (ep->caller_tid == -1) {     /* no request staged yet: wait */
        ep->recv_tid = thread_current_id();
        thread_block();

        /* A last-handle close wakes us with no caller staged. */
        if (!ep->active) return E_CANCELED;
    }

    /* The reference is held across the copies, not dropped before them: the
       request body is read out of ep->buf, and releasing first allows the
       slot to be reclaimed and wiped underneath the copy. */
    uint32_t ml = ep->msg_len;
    if (ml > a.cap) ml = a.cap;
    int rc = user_copy_out(a.buf, ep->buf, ml);

    /* The sender's pid, written only if the body made it out. A failure here
       loses the message, as it does in Haiku's _user_read_port_etc: the
       request has been taken off the endpoint by then and there is nowhere to
       put it back. The up-front check above is what keeps that narrow. */
    if (rc == 0 && a.sender)
        rc = user_copy_out(a.sender, &ep->caller_pid, sizeof(int));

    return rc != 0 ? E_BAD_ADDRESS : (int)ml;
}

int ipc_recv(uint32_t ua) {
    struct ipc_recv_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV, NULL);
    if (!ep) return E_BAD_HANDLE;
    int rc = recv_on(ep, &a);
    ep_put(ep);
    return rc;
}

/* owner side: stage the reply + wake the blocked caller. */
static int reply_on(struct endpoint *ep, const struct ipc_reply_args *ap) {
    struct ipc_reply_args a = *ap;
    if (ep->caller_tid == -1) return E_INVALID_ARGS;   /* nobody waiting */
    if (a.len > sizeof ep->buf) return E_OUT_OF_RANGE;

    /* Read in the owner's space. A failure leaves ep->buf partially written
       and the caller still blocked, which is what it did before this change
       too: whether a failed reply should abort the transaction and wake the
       caller with an error is a behaviour decision, not a copy-safety one. */
    if (user_copy_in(ep->buf, a.data, a.len) != 0) return E_BAD_ADDRESS;
    ep->msg_len = a.len;

    thread_unblock(ep->caller_tid);   /* caller resumes in ipc_call, copies reply */
    return 0;
}

int ipc_reply(uint32_t ua) {
    struct ipc_reply_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV, NULL);
    if (!ep) return E_BAD_HANDLE;
    int rc = reply_on(ep, &a);
    ep_put(ep);
    return rc;
}
