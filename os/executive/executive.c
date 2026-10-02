// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /os/executive/executive.c */
/* Aurora Tejeda / CATX Systems */
/*
 * CXK system executive (.xoex) - the broker. Proves the brokered model end to
 * end: it spawns a capability-less app, then serves the app's request over IPC -
 * performing the privileged console write the app itself is forbidden to do.
 *   ep_create -> spawn(app) -> ipc_recv (block) -> console_write(app's msg)
 *   -> ipc_reply -> yield (let the app finish) -> exit.
 */

#include "cxk_abi.h"
#include "app_image.h"   /* shell_xsex[], shell_xsex_len */

static inline void sys_write(const char *s) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_CONSOLE_WRITE), "b"(s), "c"(0) : "memory");
}
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    for (;;) { }
}
static inline int sys_ep_create(void) {
    int r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_EP_CREATE) : "memory"); return r;
}
static inline int sys_spawn(struct spawn_args *a) {
    int r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_SPAWN), "b"(a) : "memory"); return r;
}
static inline int sys_exec_path(const char *p, struct spawn_args *a) {
    int r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_EXEC_PATH), "b"(p), "c"(a) : "memory"); return r;
}
static inline int sys_ipc_recv(struct ipc_recv_args *a) {
    int r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_IPC_RECV), "b"(a) : "memory"); return r;
}
static inline int sys_ipc_reply(struct ipc_reply_args *a) {
    int r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_IPC_REPLY), "b"(a) : "memory"); return r;
}
static inline void sys_yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD) : "memory");
}
static inline int sys_fb_op(struct fb_op_args *a) {
    int r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_FB_OP), "b"(a) : "memory"); return r;
}

void _start(void) {
    sys_write("CXK executive online (.xoex) - brokering\n");

    int ep = sys_ep_create();
    if (ep < 0) { sys_write("executive: ep_create failed\n"); sys_exit(1); }

    struct spawn_args sa;
    sa.image = shell_xsex; sa.image_len = shell_xsex_len;
    sa.name = "shell";     sa.broker_endpoint = ep;
    sa.caps = GRANT_OS_BASELINE;   /* privileged shell (attenuated to executive's caps) */
    /* No arguments for the shell - but say so rather than leaving the fields
       as whatever was on the stack. The kernel validates args against
       args_len, so a stale pointer here is a spawn that fails with E_FAULT
       and a machine that boots to nothing. */
    sa.args = 0; sa.args_len = 0;
    int pid = sys_spawn(&sa);
    if (pid < 0) { sys_write("executive: spawn failed\n"); sys_exit(1); }

    /* Start the service supervisor. It is loaded from the disk rather than
       embedded here, so the kernel verifies its signature on the way in - the
       process that decides what else gets to run is exactly the one that
       should have to prove who it is.
     *
     * It gets GRANT_OS_BASELINE, attenuated against what this executive holds,
     * because it has to hand authority on to the services it starts and can
     * only pass a subset of its own. What each service actually receives is
     * decided by that service's descriptor, not here.
     *
     * A machine with no supervisor on disk is not a broken machine, it is a
     * machine with no services - so this is a note, not a failure. */
    struct spawn_args svc;
    svc.image = 0; svc.image_len = 0;
    svc.name = "supervisor"; svc.broker_endpoint = -1;
    svc.caps = GRANT_OS_BASELINE;
    svc.args = 0; svc.args_len = 0;
    if (sys_exec_path("/System/Programs/supervisor.xsex", &svc) < 0)
        sys_write("executive: no service supervisor (/System/Programs/supervisor.xsex)\n");

    /* serve one request from the app */
    char buf[256]; int sender = 0;
    struct ipc_recv_args ra;
    ra.ep_handle = ep; ra.buf = buf; ra.cap = sizeof buf; ra.sender = &sender;
    int n = sys_ipc_recv(&ra);          /* blocks until the app ipc_calls */
    if (n > 0) {
        if (n < (int)sizeof buf) buf[n] = 0;   /* NUL-terminate for the write */
        sys_write(buf);                  /* print the app's message FOR it */
    }

    struct ipc_reply_args rp;
    rp.ep_handle = ep; rp.data = "ok"; rp.len = 2;
    sys_ipc_reply(&rp);                  /* wake the app */

    sys_yield();                         /* let the app resume and exit */
    sys_write("CXK executive: served the app, shutting down\n");
    sys_exit(0);
}