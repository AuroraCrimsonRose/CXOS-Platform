/* /CXK/os/executive/executive.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK system executive (.xoex) - the broker. Proves the brokered model end to
 * end: it spawns a capability-less app, then serves the app's request over IPC -
 * performing the privileged console write the app itself is forbidden to do.
 *   ep_create -> spawn(app) -> ipc_recv (block) -> console_write(app's msg)
 *   -> ipc_reply -> yield (let the app finish) -> exit.
 */

#include "cxk_abi.h"
#include "app_image.h"   /* shell_xcex[], shell_xcex_len */

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
    sa.image = shell_xcex; sa.image_len = shell_xcex_len;
    sa.name = "shell";     sa.broker_endpoint = ep;
    sa.caps = CAP_OS_BASELINE;   /* privileged shell (attenuated to executive's caps) */
    int pid = sys_spawn(&sa);
    if (pid < 0) { sys_write("executive: spawn failed\n"); sys_exit(1); }

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