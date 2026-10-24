// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/ipc.h */
/* Aurora Tejeda / CATX Systems */
/*
 * ABI v1 - IPC endpoints + synchronous rendezvous (docs/CXK_ABI_v1 sec 8).
 *
 * Single in-flight per endpoint: an app ipc_calls (blocks), the owning executive
 * ipc_recvs the request, does the work with its own caps, and ipc_replies, which
 * wakes the caller. Request and reply both move through the kernel-side `buf`
 * (the heap is shared into every space), so each side copies in its own CR3.
 */

#ifndef IPC_H
#define IPC_H

#include "cxk_abi.h"
#include "object/kobj.h"
#include <stdint.h>

struct endpoint {
    struct kobj hdr;        /* first: the handle table holds &hdr */
    int      active;        /* not yet logically destroyed (see below) */
    int      owner_pid;     /* the process that created it (its quota); a pid */
    int      recv_tid;      /* the thread blocked in ipc_recv on it, or -1 */
    int      caller_tid;    /* the thread blocked in ipc_call on it, or -1 */
    int      caller_pid;    /* and its process, which ipc_recv reports */
    uint32_t reply_ptr;     /* caller's reply buffer (in the caller's space) */
    uint32_t reply_cap;     /* caller's reply buffer size */
                            /* These two are the authority for the copy-out in
                               ipc_call, re-validated there rather than
                               trusted from before the block (review §6). */
    uint32_t msg_len;       /* bytes currently staged in buf (request, then reply) */
    uint8_t  buf[4096];     /* kernel bounce buffer for request + reply */
};

/*
 * Lifetime (security review §7, engineering §10).
 *
 * An endpoint outlives any one handle to it: ep_create installs a RECV handle
 * for the owner, and every spawned process gets a SEND handle to the broker's
 * endpoint (cpu/spawn.c). Freeing on the first close would therefore be a
 * use-after-free, not merely early.
 *
 * So lifetime is the kernel object's two counts (object/kobj.h), and
 * destruction is two-phase - the shape Haiku's ports and Mach's ip_active /
 * ip_references both use:
 *
 *   refs    every handle holds one, and so does every ipc_* operation, from
 *           its lookup until it returns, across the part where it can block.
 *           The endpoint returns to its cache when this reaches zero,
 *           and not before.
 *   active  cleared by the last *handle* going (the type's last_handle hook).
 *           The slot may still be alive, because a blocked thread holds a
 *           reference, but it is closed for business: every entry point
 *           refuses, and anyone already blocked on it is woken to find that
 *           out rather than waiting forever.
 *
 * Keeping the two separate is the point. One counter cannot express "nobody
 * can reach this any more, but a thread is still standing on it".
 */

/* Per-process cap, on top of the global pool (security review §7). Without it
   one process can still take every slot and deny endpoints to everyone else,
   which is the denial a global limit alone does not prevent. */
#define MAX_ENDPOINTS_PER_PROC 8

/* Register the endpoint type. Called once during boot. */
void ipc_init(void);

/* Endpoints currently allocated. For the self-tests, which assert that a
   create/close cycle returns the slot rather than leaking it. */
int ep_in_use_count(void);
/* Is `ep` a live object of the endpoint cache? For the self-tests. */
int ep_is_cached(const struct endpoint *ep);

/* ep_create: allocate an endpoint owned by the caller + install a RECV handle.
   Returns the handle, or a negative ABI error. (GRANT_ENDPOINT, checked by
   the dispatcher.) */
int ep_create(void);

/* Resolve one of the caller's handles to its endpoint, requiring
   `need_rights`. The endpoint comes back with a reference the caller must
   drop with ep_put. `grant`, if given, receives the handle's grant id. */
struct endpoint *ep_from_handle(int32_t h, uint32_t need_rights, uint32_t *grant);
void ep_put(struct endpoint *ep);

/* Give the process of thread `tid` a handle on an existing, active endpoint,
   derived from the grant `parent_grant`. Returns the handle, or a negative
   ABI error. */
int32_t ep_install_handle(int tid, struct endpoint *ep, uint32_t rights, uint32_t parent_grant);

/* the synchronous rendezvous (each takes a user pointer to its args struct) */
int ipc_call(uint32_t ua);    /* caller (app); blocks -> reply len */
int ipc_recv(uint32_t ua);    /* owner (executive); blocks -> req len */
int ipc_reply(uint32_t ua);  /* owner; wakes the caller -> 0 */

#endif
