/* /CXK/abi/cxk_abi.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK ABI v1 - the PUBLIC contract shared by the kernel, the .xoex executive,
 * and .xcex apps (and, later, the X toolchain). This is the single source of
 * truth for syscall numbers, error codes, and shared call structures: kernel
 * and user code compile against the SAME definitions, so they can never drift.
 *
 * Calling convention (see docs/CXK_ABI_v1):
 *   eax = syscall number   ebx,ecx,edx,esi,edi = args 1..5   -> int 0x80
 *   result in eax: >= 0 success (call-specific), < 0 a negative error code.
 *   all GP registers except eax are preserved across the call.
 *   64-bit values are passed by pointer to a struct, never split across regs.
 */

#ifndef CXK_ABI_H
#define CXK_ABI_H

#include <stdint.h>

/* ---- syscall numbers ---- */
/* lifecycle 0x00-0x0F (no capability required) */
#define SYS_EXIT          0x00   /* ebx = exit code; does not return */
#define SYS_YIELD         0x01   /* cooperatively yield the CPU */
#define SYS_GETPID        0x02   /* -> caller pid */
#define SYS_GETUID        0x03   /* -> caller owning uid (0 = SYSTEM) */
/* IPC + handles 0x10-0x1F */
#define SYS_IPC_CALL      0x10   /* ebx = *ipc_call_args -> reply length (blocks) */
#define SYS_IPC_RECV      0x11   /* ebx = *ipc_recv_args -> request length (blocks) */
#define SYS_IPC_REPLY     0x12   /* ebx = *ipc_reply_args -> 0 */
#define SYS_EP_CREATE     0x13   /* create an endpoint -> RECV handle (CAP_ENDPOINT) */
#define SYS_HANDLE_CLOSE  0x16   /* release a handle */
/* console 0x30-0x3F (privileged: CAP_CONSOLE) */
#define SYS_CONSOLE_WRITE 0x30   /* ebx = buf, ecx = len (0 = bounded NUL-scan) */
/* process 0x70-0x7F (privileged) */
#define SYS_SPAWN         0x70   /* ebx = *spawn_args (CAP_SPAWN) -> pid */

/* ---- error codes (negative; returned in eax) ---- */
#define E_OK       0
#define E_PERM   (-1)    /* capability denied */
#define E_INVAL  (-2)    /* bad argument */
#define E_FAULT  (-3)    /* bad/unmapped user pointer */
#define E_NOENT  (-4)    /* no such object */
#define E_NOMEM  (-5)    /* out of memory */
#define E_BADF   (-6)    /* bad handle */
#define E_AGAIN  (-7)    /* would block / no message ready */
#define E_RANGE  (-8)    /* message too large / buffer too small */
#define E_NOSYS  (-9)    /* unknown syscall number */

/* ---- shared call structures ---- */
struct spawn_args {
    const void *image;            /* the app's CXEX bytes (in the caller's space) */
    uint32_t    image_len;
    const char *name;             /* optional; not dereferenced in v1 */
    int         broker_endpoint;  /* a RECV endpoint handle the caller owns */
};

/* IPC is synchronous: ipc_call blocks the caller until the owner ipc_replies.
   Messages are bounded (<= one page) and copied through the kernel. The message
   schema is defined by the executive; the kernel just moves bytes. */
struct ipc_call_args {
    int         ep_handle;   /* a SEND endpoint handle (the broker channel) */
    const void *req;         /* request bytes */
    uint32_t    req_len;
    void       *reply;       /* buffer the reply is copied into */
    uint32_t    reply_cap;
};
struct ipc_recv_args {
    int       ep_handle;     /* a RECV endpoint handle (owner side) */
    void     *buf;           /* request copied here */
    uint32_t  cap;
    int      *sender;        /* set to the caller's pid (may be 0) */
};
struct ipc_reply_args {
    int         ep_handle;   /* the RECV endpoint just received on */
    const void *data;        /* reply bytes */
    uint32_t    len;
};

#endif