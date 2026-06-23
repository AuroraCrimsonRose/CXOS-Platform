/* /CXK/kernel/cpu/handle.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * ABI v1 - per-process handle table (see docs/CXK_ABI_v1 sec 6).
 *
 * A handle is a small non-negative integer index into a process's table. The
 * object it names lives in the kernel; holding the handle is the authority to
 * use that object. v1 stores exactly one object type, HANDLE_ENDPOINT - the
 * table mechanism is what scales to files / shared memory / more endpoints
 * later; limiting the contents to endpoints is the v1 simplification.
 *
 * The ops here are pure (they operate on a caller-supplied table), so they are
 * unit-testable in isolation; sched.c wraps them against each thread's table.
 */

#ifndef HANDLE_H
#define HANDLE_H

#include <stdint.h>

enum handle_type {
    HANDLE_NONE     = 0,    /* empty slot */
    HANDLE_ENDPOINT = 1     /* IPC endpoint (v1) */
};

/* handle rights (per slot) */
#define HRIGHT_SEND  0x1u   /* may ipc_call through this endpoint (app side) */
#define HRIGHT_RECV  0x2u   /* may ipc_recv/ipc_reply on this endpoint (owner) */

#define CXK_MAX_HANDLES 16   /* handle slots per process */

struct cap_handle {
    uint8_t  type;          /* enum handle_type */
    uint8_t  rights;        /* HRIGHT_* */
    uint16_t _pad;
    void    *object;        /* kernel object (e.g. struct endpoint *) */
};

/* Install an object into the first free slot of `tbl` (size `max`).
   Returns the handle index, or -1 if the table is full. */
int handle_install(struct cap_handle *tbl, int max,
                   uint8_t type, uint8_t rights, void *object);

/* Look up a handle. Returns the slot, or NULL if idx is out of range / empty. */
struct cap_handle *handle_get(struct cap_handle *tbl, int max, int idx);

/* Release a handle slot. Returns 0 on success, -1 if idx is invalid/empty.
   (Does not free the underlying object - object lifetime is the kernel's.) */
int handle_close(struct cap_handle *tbl, int max, int idx);

#endif