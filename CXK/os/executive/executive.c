/* /CXK/os/executive/executive.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK system executive (.xoex) - the broker. It holds privileged capabilities;
 * its apps hold none. This bootstrap proves the brokered model end to end:
 *   1. create an IPC endpoint (CAP_ENDPOINT)        -> broker channel
 *   2. spawn a capability-less .xcex app (CAP_SPAWN) -> app pid
 *   3. yield so the app runs; the app tries to write the console directly and
 *      the kernel DENIES it (E_PERM) - it has no CAP_CONSOLE. The app exits.
 *   4. the executive (which DOES hold CAP_CONSOLE) prints and exits.
 * Syscall numbers come from the shared public ABI header.
 */

#include "cxk_abi.h"
#include "app_image.h"   /* hello_xcex[], hello_xcex_len */

static inline void sys_write(const char *s) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_CONSOLE_WRITE), "b"(s), "c"(0) : "memory");
}
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    for (;;) { }
}
static inline int sys_ep_create(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_EP_CREATE) : "memory");
    return r;
}
static inline int sys_spawn(struct spawn_args *a) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_SPAWN), "b"(a) : "memory");
    return r;
}
static inline void sys_yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD) : "memory");
}

void _start(void) {
    sys_write("CXK executive online (.xoex) - brokering\n");

    int ep = sys_ep_create();
    if (ep < 0) { sys_write("executive: ep_create failed\n"); sys_exit(1); }

    struct spawn_args a;
    a.image          = hello_xcex;
    a.image_len      = hello_xcex_len;
    a.name           = "hello";
    a.broker_endpoint = ep;
    int pid = sys_spawn(&a);
    if (pid < 0) { sys_write("executive: spawn failed\n"); sys_exit(1); }

    sys_yield();   /* let the app run (it will be denied the console) */

    sys_write("CXK executive: app returned, shutting down\n");
    sys_exit(0);
}