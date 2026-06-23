/* /CXK/kernel/cpu/ipc.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * ABI v1 - IPC endpoints (see docs/CXK_ABI_v1 sec 8).
 *
 * CP2a: endpoint objects + ep_create (the executive mints an endpoint and gets
 * a RECV handle). The synchronous rendezvous (ipc_call/recv/reply) lands in CP3;
 * its per-endpoint state is declared here now so CP3 doesn't reshape the struct.
 */

#ifndef IPC_H
#define IPC_H

#include <stdint.h>

struct endpoint {
    int in_use;
    int owner_pid;        /* holder of the RECV side (the broker) */

    /* ---- CP3 synchronous-rendezvous state (reserved; unused in CP2a) ---- */
    int      caller_pid;  /* a blocked ipc_call caller, or -1 */
    void    *reply_ptr;   /* caller's reply buffer (validated at call time) */
    uint32_t reply_cap;   /* caller's reply buffer size */
    uint32_t req_len;     /* bytes staged in req_buf */
    uint8_t  req_buf[4096];
};

/* ep_create: allocate an endpoint owned by the caller and install a RECV handle
   for it in the caller's handle table. Returns the handle index, or a negative
   ABI error. Caller must hold CAP_ENDPOINT (checked by the dispatcher). */
int ep_create(void);

/* CP3 helper: resolve a handle index to its endpoint, requiring `need_rights`
   (HRIGHT_SEND or HRIGHT_RECV). Returns NULL on bad handle / wrong type/rights. */
struct endpoint *ep_from_handle(int idx, uint8_t need_rights);

#endif