// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/ipc.h */
/* Aurora Tejeda / CATX Systems */
/*
 * IPC: endpoints, send handles with badges, queued calls with reply tokens
 * (roadmap step 1.3a; docs/kernel/CX_KERNEL_PLAN.md section 6).
 *
 * Three object types:
 *
 *   endpoint     what a server receives on. Only receive handles name it
 *                (HRIGHT_RECV), so its last handle going is the server going.
 *   send handle  what a client calls through: the endpoint and a badge its
 *                minter chose (SYS_EP_MINT; spawn's broker handle has 0). A
 *                server tells its clients apart by the badge, which a client
 *                can neither forge nor change: it is in the kernel's object,
 *                not the message (seL4 badged endpoint capabilities).
 *   reply token  one call, from the moment it is made until it is answered.
 *                It waits on the endpoint's queue, oldest first; ipc_recv takes
 *                it off and gives the server a handle on it, which ipc_reply
 *                answers once and closes (seL4 reply objects, Mach send-once
 *                rights). A server holds as many as it likes and answers them
 *                in any order.
 *
 * Every way a call can end:
 *
 *   answered        ipc_reply: the caller copies the reply out.
 *   token closed    the server closed it unanswered, or ended: E_PEER_CLOSED.
 *   server gone     the endpoint's last receive handle closed: every queued
 *                   call fails E_PEER_CLOSED, and a new one is refused. Calls
 *                   already taken stay with their tokens.
 *   caller killed   a queued call is withdrawn; a taken one is marked, and its
 *                   reply finds the caller gone (E_PEER_CLOSED to the server).
 *
 * A call's bytes, request and then reply, sit in a kernel buffer the size of
 * the larger of the two, charged to the caller's job until the token goes.
 * One lock (ipc_lock in ipc.c) covers every endpoint's queue and every
 * token's state; it is taken before the wait lock, never after, and before a
 * handle table's lock (htab_take), whose holders run no hooks.
 *
 * Handles travel beside the bytes (roadmap 1.3b), up to IPC_HANDLES_MAX each
 * way: taken from the sender's table all or none, under ipc_lock as the call
 * is queued or the reply finished, and landed in the receiver's all or none.
 * In between they are transits in the call (object/htab.h), and a call that
 * ends unread closes them. A message never carries the endpoint or token it
 * goes through: an endpoint holding its own receive handle could never close.
 */

#ifndef IPC_H
#define IPC_H

#include "cxk_abi.h"
#include "object/kobj.h"
#include "object/htab.h"
#include <stdint.h>

struct job;

/* Where a call is. Only the lock holder moves it, and DONE is final. */
enum ipc_state {
    IPC_QUEUED   = 0,   /* on the endpoint's queue, not yet received */
    IPC_TAKEN    = 1,   /* received: the server holds its token */
    IPC_REPLYING = 2,   /* a reply is being copied in; nothing else may touch buf */
    IPC_DONE     = 3,   /* result set; the caller may read it */
};

struct ipc_call {
    struct kobj      hdr;         /* first; KOBJ_REPLY */
    struct ipc_call *next;        /* on the endpoint's queue while QUEUED */
    struct endpoint *ep;          /* the endpoint it was made on, while QUEUED */
    enum ipc_state   state;
    int              caller_tid;
    int              caller_pid;  /* what ipc_recv reports */
    int              caller_gone; /* the caller has left; nobody reads the result */
    int32_t          result;      /* DONE: the reply's length, or a negative error */
    uint64_t         badge;
    uint32_t         req_len;
    uint32_t         reply_cap;
    uint32_t         size;        /* of buf */
    uint8_t         *buf;         /* the request, then the reply; freed with the token */
    struct job      *job;         /* charged `size` bytes; a reference */
    /* Handles in transit, like buf: the request's until ipc_recv lands them,
       then the reply's until the caller does. Dropped with the call. */
    uint32_t         nh;
    uint32_t         reply_hcap;  /* the most the caller takes back */
    struct htab_transit h[IPC_HANDLES_MAX];
    /* Priority (ipc.c): the thread that took it and answers for it, -1 for
       none; the endpoint it was taken from, until that closes; and the list
       of calls that have a server. */
    int              server_tid;
    struct endpoint *from;
    struct ipc_call *serve_next;
};

struct endpoint {
    struct kobj      hdr;         /* first: the handle table holds &hdr */
    int              active;      /* a receive handle is left (see below) */
    int              owner_pid;   /* the process that created it (its quota); a pid */
    struct ipc_call *head;        /* queued calls, oldest first */
    struct ipc_call *tail;
    uint32_t         queued;
    /* A stop notice waiting for ipc_recv (ep_post_stop), which takes it
       before any call: who asked, and the deadline they gave. */
    int              stop_pending;
    uint32_t         stop_by;
    uint32_t         stop_ms;
};

struct ep_send {
    struct kobj      hdr;         /* first; KOBJ_SEND */
    struct endpoint *ep;          /* a reference */
    uint64_t         badge;
};

/*
 * Lifetime (security review §7, engineering §10).
 *
 * An endpoint outlives any one handle to it: ep_create installs a RECV handle
 * for the owner, every send handle holds a reference, and so does every
 * ipc_* operation, from its lookup until it returns, across the part where
 * it can block. Freeing on the first close would be a use-after-free.
 *
 * So lifetime is the kernel object's two counts (object/kobj.h), and
 * destruction is two-phase - the shape Haiku's ports and Mach's ip_active /
 * ip_references both use:
 *
 *   refs    the endpoint returns to its cache when this reaches zero, and
 *           not before.
 *   active  cleared by the last *receive handle* going (the type's
 *           last_handle hook; send handles are handles on another object).
 *           The slot may still be alive, because a send handle or a blocked
 *           thread holds a reference, but it is closed for business: every
 *           entry point refuses, and anyone already blocked on it is woken to
 *           find that out rather than waiting forever.
 */

/* Per-process cap, on top of the global pool (security review §7). Without it
   one process can still take every slot and deny endpoints to everyone else,
   which is the denial a global limit alone does not prevent. */
#define MAX_ENDPOINTS_PER_PROC 8

/* Register the three types. Called once during boot. */
void ipc_init(void);

/* Endpoints currently allocated. For the self-tests, which assert that a
   create/close cycle returns the slot rather than leaking it. */
int ep_in_use_count(void);
/* Is `ep` a live object of the endpoint cache? For the self-tests. */
int ep_is_cached(const struct endpoint *ep);
/* Calls queued on `ep` now, for the self-tests. */
uint32_t ep_queued(const struct endpoint *ep);

/* ep_create: allocate an endpoint owned by the caller + install a RECV handle.
   Returns the handle, or a negative ABI error. (GRANT_ENDPOINT, checked by
   the dispatcher.) */
int ep_create(void);

/* Resolve one of the caller's endpoint handles, requiring `need_rights`. The
   endpoint comes back with a reference the caller must drop with ep_put.
   `grant`, if given, receives the handle's grant id. */
struct endpoint *ep_from_handle(int32_t h, uint32_t need_rights, uint32_t *grant);
void ep_put(struct endpoint *ep);

/* Give the process of thread `tid` a further handle on an existing, active
   endpoint, derived from the grant `parent_grant`: `rights` from HRIGHT_RECV,
   HRIGHT_WAIT, HRIGHT_DUPLICATE. Returns the handle, or a negative ABI error. */
int32_t ep_install_handle(int tid, struct endpoint *ep, uint32_t rights, uint32_t parent_grant);

/* Give it a send handle on `ep` carrying `badge`, with `rights` (HRIGHT_SEND,
   and HRIGHT_DUPLICATE if asked). Returns the handle, or a negative ABI
   error: E_BAD_HANDLE for an endpoint whose server has gone. */
int32_t ep_install_send(int tid, struct endpoint *ep, uint64_t badge, uint32_t rights, uint32_t parent_grant);

/* SYS_EP_MINT: a send handle on the caller's endpoint `h` (HRIGHT_RECV). */
int ep_mint(int32_t h, uint64_t badge, uint32_t rights);

/* Leave a stop notice on `ep` for the process `by`, which gave `ms`, and wake
   its receiver (SYS_PROCESS_STOP). A later one replaces it. 0, or
   E_PEER_CLOSED if the endpoint's last handle has gone: nobody can read it. */
int ep_post_stop(struct endpoint *ep, int by, uint32_t ms);

/* The calls (each takes a user pointer to its args struct). */
int ipc_call(uint64_t ua);    /* client; blocks -> reply length */
int ipc_recv(uint64_t ua);    /* server; blocks -> message length */
int ipc_reply(uint64_t ua);   /* server; answers a token -> 0 */

/* Priority through calls (ipc.c). ipc_set_base gives thread `tid` its own
   level and passes the change down any chain of calls it is in; the checks
   on who may are the caller's. ipc_thread_exit: the calls `tid` took stop
   lifting it (thread_exit, before the id can be reused). */
void ipc_set_base(int tid, uint32_t level);
void ipc_thread_exit(int tid);
uint32_t ipc_serving_count(void);   /* calls that lift a server, for the self-tests */

#endif
