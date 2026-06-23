/* /CXK/kernel/cpu/ipc.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* ABI v1 IPC endpoints - CP2a (creation + handle wiring). See ipc.h. */

#include "ipc.h"
#include "handle.h"
#include "sched.h"
#include "caps.h"
#include <stddef.h>

#define MAX_ENDPOINTS 32
static struct endpoint endpoints[MAX_ENDPOINTS];

int ep_create(void) {
    int me = thread_current_id();

    int slot = -1;
    for (int i = 0; i < MAX_ENDPOINTS; i++) {
        if (!endpoints[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return E_NOMEM;

    struct endpoint *ep = &endpoints[slot];
    ep->in_use     = 1;
    ep->owner_pid  = me;
    ep->caller_pid = -1;          /* no blocked caller (CP3) */
    ep->reply_ptr  = NULL;
    ep->reply_cap  = 0;
    ep->req_len    = 0;

    int h = thread_handle_install(me, HANDLE_ENDPOINT, HRIGHT_RECV, ep);
    if (h < 0) { ep->in_use = 0; return E_NOMEM; }   /* handle table full */
    return h;
}

struct endpoint *ep_from_handle(int idx, uint8_t need_rights) {
    struct cap_handle *h = thread_handle_get(thread_current_id(), idx);
    if (!h || h->type != HANDLE_ENDPOINT) return NULL;
    if ((h->rights & need_rights) != need_rights) return NULL;
    return (struct endpoint *)h->object;
}