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
    if (!user_ptr_readable((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct ipc_call_args a = *ua;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_SEND);
    if (!ep) return E_BADF;
    if (a.req_len > sizeof ep->buf) return E_RANGE;
    if (a.req_len && !user_ptr_readable((uint32_t)a.req, a.req_len))         return E_FAULT;
    if (a.reply_cap && !user_ptr_writable((uint32_t)a.reply, a.reply_cap))   return E_FAULT;
    if (ep->caller_pid != -1) return E_AGAIN;   /* single in-flight (v1) */

    for (uint32_t i = 0; i < a.req_len; i++)            /* read req in caller space */
        ep->buf[i] = ((const uint8_t *)a.req)[i];
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

    uint32_t rl = ep->msg_len;
    if (rl > a.reply_cap) rl = a.reply_cap;
    for (uint32_t i = 0; i < rl; i++)                    /* write reply in caller space */
        ((uint8_t *)a.reply)[i] = ep->buf[i];
    ep->caller_pid = -1;   /* rendezvous complete */
    ep_release(ep);
    return (int)rl;
}

/* owner side: wait for a caller, then copy the staged request out. Runs in the
   owner's (executive's) address space. */
int ipc_recv(const struct ipc_recv_args *ua) {
    if (!user_ptr_readable((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct ipc_recv_args a = *ua;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV);
    if (!ep) return E_BADF;
    if (a.cap && !user_ptr_writable((uint32_t)a.buf, a.cap)) return E_FAULT;

    ep_acquire(ep);                    /* see ipc_call: held across the block */
    while (ep->caller_pid == -1) {     /* no request staged yet: wait */
        ep->recv_blocked = 1;
        thread_block();

        /* A last-handle close wakes us with no caller staged. */
        if (!ep->active) { ep_release(ep); return E_BADF; }
    }
    ep_release(ep);

    uint32_t ml = ep->msg_len;
    if (ml > a.cap) ml = a.cap;
    for (uint32_t i = 0; i < ml; i++)
        ((uint8_t *)a.buf)[i] = ep->buf[i];
    if (a.sender && user_ptr_writable((uint32_t)a.sender, sizeof(int)))
        *a.sender = ep->caller_pid;
    return (int)ml;
}

/* owner side: stage the reply + wake the blocked caller. */
int ipc_reply(const struct ipc_reply_args *ua) {
    if (!user_ptr_readable((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct ipc_reply_args a = *ua;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV);
    if (!ep) return E_BADF;
    if (ep->caller_pid == -1) return E_INVAL;   /* nobody waiting */
    if (a.len > sizeof ep->buf) return E_RANGE;
    if (a.len && !user_ptr_readable((uint32_t)a.data, a.len)) return E_FAULT;

    for (uint32_t i = 0; i < a.len; i++)        /* read reply in owner space */
        ep->buf[i] = ((const uint8_t *)a.data)[i];
    ep->msg_len = a.len;

    thread_unblock(ep->caller_pid);   /* caller resumes in ipc_call, copies reply */
    return 0;
}