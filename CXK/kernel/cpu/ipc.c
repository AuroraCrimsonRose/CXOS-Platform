/* /CXK/kernel/cpu/ipc.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* ABI v1 IPC: endpoints + synchronous call/recv/reply rendezvous. See ipc.h. */

#include "ipc.h"
#include "handle.h"
#include "sched.h"
#include "caps.h"
#include "usermode.h"   /* user_ptr_ok */
#include <stddef.h>

#define MAX_ENDPOINTS 32
static struct endpoint endpoints[MAX_ENDPOINTS];

int ep_create(void) {
    int me = thread_current_id();
    int slot = -1;
    for (int i = 0; i < MAX_ENDPOINTS; i++)
        if (!endpoints[i].in_use) { slot = i; break; }
    if (slot < 0) return E_NOMEM;

    struct endpoint *ep = &endpoints[slot];
    ep->in_use       = 1;
    ep->owner_pid    = me;
    ep->recv_blocked = 0;
    ep->caller_pid   = -1;
    ep->reply_ptr    = NULL;
    ep->reply_cap    = 0;
    ep->msg_len      = 0;

    int h = thread_handle_install(me, HANDLE_ENDPOINT, HRIGHT_RECV, ep);
    if (h < 0) { ep->in_use = 0; return E_NOMEM; }
    return h;
}

struct endpoint *ep_from_handle(int idx, uint8_t need_rights) {
    struct cap_handle *h = thread_handle_get(thread_current_id(), idx);
    if (!h || h->type != HANDLE_ENDPOINT) return NULL;
    if ((h->rights & need_rights) != need_rights) return NULL;
    return (struct endpoint *)h->object;
}

/* app side: stage the request, wake a waiting owner, block until replied, then
   copy the reply out. Runs in the caller's address space (its CR3 is live). */
int ipc_call(const struct ipc_call_args *ua) {
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct ipc_call_args a = *ua;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_SEND);
    if (!ep) return E_BADF;
    if (a.req_len > sizeof ep->buf) return E_RANGE;
    if (a.req_len && !user_ptr_ok((uint32_t)a.req, a.req_len))         return E_FAULT;
    if (a.reply_cap && !user_ptr_ok((uint32_t)a.reply, a.reply_cap))   return E_FAULT;
    if (ep->caller_pid != -1) return E_AGAIN;   /* single in-flight (v1) */

    for (uint32_t i = 0; i < a.req_len; i++)            /* read req in caller space */
        ep->buf[i] = ((const uint8_t *)a.req)[i];
    ep->msg_len    = a.req_len;
    ep->reply_ptr  = a.reply;
    ep->reply_cap  = a.reply_cap;
    ep->caller_pid = thread_current_id();

    if (ep->recv_blocked) { ep->recv_blocked = 0; thread_unblock(ep->owner_pid); }

    thread_block();   /* until ipc_reply wakes us; reply now staged in ep->buf */

    uint32_t rl = ep->msg_len;
    if (rl > a.reply_cap) rl = a.reply_cap;
    for (uint32_t i = 0; i < rl; i++)                    /* write reply in caller space */
        ((uint8_t *)a.reply)[i] = ep->buf[i];
    ep->caller_pid = -1;   /* rendezvous complete */
    return (int)rl;
}

/* owner side: wait for a caller, then copy the staged request out. Runs in the
   owner's (executive's) address space. */
int ipc_recv(const struct ipc_recv_args *ua) {
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct ipc_recv_args a = *ua;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV);
    if (!ep) return E_BADF;
    if (a.cap && !user_ptr_ok((uint32_t)a.buf, a.cap)) return E_FAULT;

    while (ep->caller_pid == -1) {     /* no request staged yet: wait */
        ep->recv_blocked = 1;
        thread_block();
    }

    uint32_t ml = ep->msg_len;
    if (ml > a.cap) ml = a.cap;
    for (uint32_t i = 0; i < ml; i++)
        ((uint8_t *)a.buf)[i] = ep->buf[i];
    if (a.sender && user_ptr_ok((uint32_t)a.sender, sizeof(int)))
        *a.sender = ep->caller_pid;
    return (int)ml;
}

/* owner side: stage the reply + wake the blocked caller. */
int ipc_reply(const struct ipc_reply_args *ua) {
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct ipc_reply_args a = *ua;

    struct endpoint *ep = ep_from_handle(a.ep_handle, HRIGHT_RECV);
    if (!ep) return E_BADF;
    if (ep->caller_pid == -1) return E_INVAL;   /* nobody waiting */
    if (a.len > sizeof ep->buf) return E_RANGE;
    if (a.len && !user_ptr_ok((uint32_t)a.data, a.len)) return E_FAULT;

    for (uint32_t i = 0; i < a.len; i++)        /* read reply in owner space */
        ep->buf[i] = ((const uint8_t *)a.data)[i];
    ep->msg_len = a.len;

    thread_unblock(ep->caller_pid);   /* caller resumes in ipc_call, copies reply */
    return 0;
}