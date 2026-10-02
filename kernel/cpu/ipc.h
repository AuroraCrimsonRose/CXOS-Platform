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
#include <stdint.h>

struct endpoint {
    int      in_use;
    int      owner_pid;     /* holder of the RECV side (the broker) */
    int      recv_blocked;  /* owner is blocked in ipc_recv waiting for a caller */
    int      caller_pid;    /* a blocked ipc_call caller, or -1 */
    void    *reply_ptr;     /* caller's reply buffer (in the caller's space) */
    uint32_t reply_cap;     /* caller's reply buffer size */
    uint32_t msg_len;       /* bytes currently staged in buf (request, then reply) */
    uint8_t  buf[4096];     /* kernel bounce buffer for request + reply */
};

/* ep_create: allocate an endpoint owned by the caller + install a RECV handle.
   Returns the handle index, or a negative ABI error. (GRANT_ENDPOINT, checked by
   the dispatcher.) */
int ep_create(void);

/* resolve a handle index to its endpoint, requiring `need_rights`. */
struct endpoint *ep_from_handle(int idx, uint8_t need_rights);

/* the synchronous rendezvous (each takes a user pointer to its args struct) */
int ipc_call(const struct ipc_call_args *ua);    /* caller (app); blocks -> reply len */
int ipc_recv(const struct ipc_recv_args *ua);    /* owner (executive); blocks -> req len */
int ipc_reply(const struct ipc_reply_args *ua);  /* owner; wakes the caller -> 0 */

#endif