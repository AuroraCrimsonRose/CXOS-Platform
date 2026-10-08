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
#include "handle.h"
#include <stdint.h>

struct endpoint {
    int      in_use;        /* slot allocated out of the global pool */
    int      active;        /* not yet logically destroyed (see below) */
    int      refs;          /* handles + in-flight operations referencing this */
    int      handles;       /* handles alone; the last one going destroys it */
    int      owner_pid;     /* holder of the RECV side (the broker) */
    int      recv_blocked;  /* owner is blocked in ipc_recv waiting for a caller */
    int      caller_pid;    /* a blocked ipc_call caller, or -1 */
    void    *reply_ptr;     /* caller's reply buffer (in the caller's space) */
    uint32_t reply_cap;     /* caller's reply buffer size */
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
 * So lifetime is a count, and destruction is two-phase - the shape Haiku's
 * ports and Mach's ip_active / ip_references both use:
 *
 *   refs    every handle holds one, and every in-flight ipc_* operation holds
 *           one across the part where it can block. The slot returns to the
 *           pool when this reaches zero, and not before.
 *   active  cleared the moment the last *handle* goes. The slot may still be
 *           alive, because a blocked thread holds a reference, but it is
 *           closed for business: every entry point refuses, and anyone already
 *           blocked on it is woken to find that out rather than waiting
 *           forever.
 *
 * Keeping the two separate is the point. One counter cannot express "nobody
 * can reach this any more, but a thread is still standing on it".
 */

/* Per-process cap, on top of the global pool (security review §7). Without it
   one process can still take every slot and deny endpoints to everyone else,
   which is the denial a global limit alone does not prevent. */
#define MAX_ENDPOINTS_PER_PROC 8

/* Take a reference. The endpoint must already hold one. */
void ep_acquire(struct endpoint *ep);

/* Drop a reference, returning the slot to the pool at zero. */
void ep_release(struct endpoint *ep);

/* Handle-close hook, registered with handle_set_release(HANDLE_ENDPOINT). */
void ep_handle_release(struct cap_handle *h);

/* Register that hook. Called once during boot. */
void ipc_init(void);

/* Endpoints currently allocated. For the self-tests, which assert that a
   create/close cycle returns the slot rather than leaking it. */
int ep_in_use_count(void);

/* ep_create: allocate an endpoint owned by the caller + install a RECV handle.
   Returns the handle index, or a negative ABI error. (GRANT_ENDPOINT, checked by
   the dispatcher.) */
int ep_create(void);

/* resolve a handle index to its endpoint, requiring `need_rights`. */
struct endpoint *ep_from_handle(int idx, uint8_t need_rights);

/* Install an additional handle onto an existing endpoint, counting it. Use
   this rather than thread_handle_install, which would create a handle the
   endpoint does not know about and so cannot outlive. */
int ep_install_handle(int pid, struct endpoint *ep, uint8_t rights);

/* the synchronous rendezvous (each takes a user pointer to its args struct) */
int ipc_call(const struct ipc_call_args *ua);    /* caller (app); blocks -> reply len */
int ipc_recv(const struct ipc_recv_args *ua);    /* owner (executive); blocks -> req len */
int ipc_reply(const struct ipc_reply_args *ua);  /* owner; wakes the caller -> 0 */

#endif