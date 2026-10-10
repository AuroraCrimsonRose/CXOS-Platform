// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/ipc.c */
/* Aurora Tejeda / CATX Systems */
/* ABI v1 IPC: endpoints + synchronous call/recv/reply rendezvous. See ipc.h. */

#include "ipc.h"
#include "handle.h"
#include "sched.h"
#include "caps.h"
#include "usermode.h"   /* user_ptr_readable / user_ptr_writable */
#include <stddef.h>

#define MAX_ENDPOINTS 32
static struct endpoint endpoints[MAX_ENDPOINTS];

int ep_in_use_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENDPOINTS; i++)
        if (endpoints[i].in_use) n++;
    return n;
}

void ep_acquire(struct endpoint *ep) {
    if (!ep || !ep->in_use) return;
    ep->refs++;
}

void ep_release(struct endpoint *ep) {
    if (!ep || !ep->in_use) return;
    if (ep->refs > 0) ep->refs--;
    if (ep->refs > 0) return;

    /* Last reference: the slot goes back to the pool. Wiping buf is not
       tidiness - it has held request and reply bodies, and the next owner of
       this slot is a different process. */
    for (uint32_t i = 0; i < sizeof ep->buf; i++) ep->buf[i] = 0;
    ep->in_use       = 0;
    ep->active       = 0;
    ep->handles      = 0;
    ep->owner_pid    = -1;
    ep->recv_blocked = 0;
    ep->caller_pid   = -1;
    ep->reply_ptr    = NULL;
    ep->reply_cap    = 0;
    ep->msg_len      = 0;
}

/* The last handle has gone. Close the endpoint for business and wake anyone
   standing on it, so a peer that closes mid-rendezvous ends the other side
   with an error instead of a permanent block. The slot itself survives until
   those woken threads drop their own references. */
static void ep_destroy(struct endpoint *ep) {
    if (!ep || !ep->active) return;
    ep->active = 0;

    if (ep->recv_blocked) {
        ep->recv_blocked = 0;
        thread_unblock(ep->owner_pid);
    }
    if (ep->caller_pid != -1)
        thread_unblock(ep->caller_pid);
}

void ep_handle_release(struct cap_handle *h) {
    if (!h || h->type != HANDLE_ENDPOINT) return;
    struct endpoint *ep = (struct endpoint *)h->object;
    if (!ep || !ep->in_use) return;

    /* A handle is the only thing that makes an endpoint reachable, so the last
       one going is what destroys it logically. In-flight operations hold their
       own references and keep the slot alive a little longer. */
    if (ep->handles > 0) ep->handles--;
    if (ep->handles == 0) ep_destroy(ep);
    ep_release(ep);
}

void ipc_init(void) {
    for (int i = 0; i < MAX_ENDPOINTS; i++) endpoints[i].owner_pid = -1;
    handle_set_release(HANDLE_ENDPOINT, ep_handle_release);
}

int ep_create(void) {
    int me = thread_current_id();

    int mine = 0;
    for (int i = 0; i < MAX_ENDPOINTS; i++)
        if (endpoints[i].in_use && endpoints[i].owner_pid == me) mine++;
    if (mine >= MAX_ENDPOINTS_PER_PROC) return E_NOMEM;

    int slot = -1;
    for (int i = 0; i < MAX_ENDPOINTS; i++)
        if (!endpoints[i].in_use) { slot = i; break; }
    if (slot < 0) return E_NOMEM;

    struct endpoint *ep = &endpoints[slot];
    ep->in_use       = 1;
    ep->active       = 1;
    ep->refs         = 1;   /* the handle installed just below */
    ep->handles      = 1;
    ep->owner_pid    = me;
    ep->recv_blocked = 0;
    ep->caller_pid   = -1;
    ep->reply_ptr    = NULL;
    ep->reply_cap    = 0;
    ep->msg_len      = 0;

    int h = thread_handle_install(me, HANDLE_ENDPOINT, HRIGHT_RECV, ep);
    if (h < 0) {
        ep->handles = 0;
        ep->active  = 0;
        ep_release(ep);      /* drops the reference taken above: frees the slot */
        return E_NOMEM;
    }
    return h;
}

/* A second handle onto an existing endpoint - spawn.c hands every child a SEND
   right to the broker's. Each handle is a reference, which is exactly why the
   first close must not free. */
int ep_install_handle(int pid, struct endpoint *ep, uint8_t rights) {
    if (!ep || !ep->in_use || !ep->active) return E_BADF;

    int h = thread_handle_install(pid, HANDLE_ENDPOINT, rights, ep);
    if (h < 0) return E_NOMEM;

    ep->handles++;
    ep_acquire(ep);
    return h;
}

struct endpoint *ep_from_handle(int idx, uint8_t need_rights) {
    struct cap_handle *h = thread_handle_get(thread_current_id(), idx);
    if (!h || h->type != HANDLE_ENDPOINT) return NULL;
    if ((h->rights & need_rights) != need_rights) return NULL;

    struct endpoint *ep = (struct endpoint *)h->object;
    /* A handle on a destroyed endpoint is a dead name: it resolves to nothing,
       rather than to a slot that may already belong to someone else. */
    if (!ep || !ep->in_use || !ep->active) return NULL;
    return ep;
}

/* app side: stage the request, wake a waiting owner, block until replied, then
   copy the reply out. Runs in the caller's address space (its CR3 is live). */
int ipc_call(const struct ipc_call_args *ua) {
    struct ipc_call_args a;
    if (user_copy_in(&a, (uint32_t)ua, sizeof a) != 0) return E_FAULT;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_SEND);
    if (!ep) return E_BADF;
    if (a.req_len > sizeof ep->buf) return E_RANGE;

    /* An early reject, not the authority: the reply buffer is validated again
       at the copy-out below, which is the check that counts. This one is here
       so a caller that passes an unusable reply buffer is told now rather than
       after a whole round trip through the broker. Haiku keeps its up-front
       IS_USER_ADDRESS check for the same reason. */
    if (a.reply_cap && !user_ptr_writable((uint32_t)a.reply, a.reply_cap)) return E_FAULT;

    /* Before anything writes ep->buf: it still holds an in-flight
       transaction's message if one is live. */
    if (ep->caller_pid != -1) return E_AGAIN;   /* single in-flight (v1) */

    if (user_copy_in(ep->buf, (uint32_t)a.req, a.req_len) != 0) return E_FAULT;
    ep->msg_len    = a.req_len;
    ep->reply_ptr  = a.reply;
    ep->reply_cap  = a.reply_cap;
    ep->caller_pid = thread_current_id();

    /* Held across the block: the owner may close its handle while we sleep,
       and without this the slot could be freed and reused under us - we would
       wake and copy a reply out of some other process's endpoint. */
    ep_acquire(ep);

    if (ep->recv_blocked) { ep->recv_blocked = 0; thread_unblock(ep->owner_pid); }

    thread_block();   /* until ipc_reply wakes us; reply now staged in ep->buf */

    /* ep_destroy also wakes us, so waking does not mean a reply arrived. */
    if (!ep->active) {
        ep->caller_pid = -1;
        ep_release(ep);
        return E_BADF;
    }

    /* The copy-out, re-validated against the endpoint's own record of the
       transaction rather than the pointer this frame validated before it
       slept (security review §6). reply_ptr/reply_cap were stored and never
       read before this: the copy used the stack copy, so the kernel kept a
       record of the reply buffer that nothing consulted.

       It is deliberately done while the reference is still held - ep_release
       may return the slot to the pool and wipe buf. */
    uint32_t rl = ep->msg_len;
    if (rl > ep->reply_cap) rl = ep->reply_cap;
    int rc = user_copy_out((uint32_t)ep->reply_ptr, ep->buf, rl);

    ep->caller_pid = -1;   /* rendezvous complete either way */
    ep_release(ep);
    return rc != 0 ? E_FAULT : (int)rl;
}

/* owner side: wait for a caller, then copy the staged request out. Runs in the
   owner's (executive's) address space. */
int ipc_recv(const struct ipc_recv_args *ua) {
    struct ipc_recv_args a;
    if (user_copy_in(&a, (uint32_t)ua, sizeof a) != 0) return E_FAULT;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV);
    if (!ep) return E_BADF;
    /* Early rejects, as in ipc_call: both buffers are validated again at the
       copies below, which are the checks that count. */
    if (a.cap && !user_ptr_writable((uint32_t)a.buf, a.cap)) return E_FAULT;
    if (a.sender && !user_ptr_writable((uint32_t)a.sender, sizeof(int))) return E_FAULT;

    ep_acquire(ep);                    /* see ipc_call: held across the block */
    while (ep->caller_pid == -1) {     /* no request staged yet: wait */
        ep->recv_blocked = 1;
        thread_block();

        /* A last-handle close wakes us with no caller staged. */
        if (!ep->active) { ep_release(ep); return E_BADF; }
    }

    /* The reference is held across the copies, not dropped before them: the
       request body is read out of ep->buf, and releasing first allows the
       slot to be reclaimed and wiped underneath the copy. (A blocked caller
       holds its own reference, so this was not reachable - but it made the
       copy's safety depend on a fact about a different function.) */
    uint32_t ml = ep->msg_len;
    if (ml > a.cap) ml = a.cap;
    int rc = user_copy_out((uint32_t)a.buf, ep->buf, ml);

    /* The sender's pid, written only if the body made it out. A failure here
       loses the message, as it does in Haiku's _user_read_port_etc: the
       request has been taken off the endpoint by then and there is nowhere to
       put it back. The up-front check above is what keeps that narrow. */
    if (rc == 0 && a.sender)
        rc = user_copy_out((uint32_t)a.sender, &ep->caller_pid, sizeof(int));

    ep_release(ep);
    return rc != 0 ? E_FAULT : (int)ml;
}

/* owner side: stage the reply + wake the blocked caller. */
int ipc_reply(const struct ipc_reply_args *ua) {
    struct ipc_reply_args a;
    if (user_copy_in(&a, (uint32_t)ua, sizeof a) != 0) return E_FAULT;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV);
    if (!ep) return E_BADF;
    if (ep->caller_pid == -1) return E_INVAL;   /* nobody waiting */
    if (a.len > sizeof ep->buf) return E_RANGE;

    /* Read in the owner's space. A failure leaves ep->buf partially written
       and the caller still blocked, which is what it did before this change
       too: whether a failed reply should abort the transaction and wake the
       caller with an error is a behaviour decision, not a copy-safety one. */
    if (user_copy_in(ep->buf, (uint32_t)a.data, a.len) != 0) return E_FAULT;
    ep->msg_len = a.len;

    thread_unblock(ep->caller_pid);   /* caller resumes in ipc_call, copies reply */
    return 0;
}