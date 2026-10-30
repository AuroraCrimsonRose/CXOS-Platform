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
#include <app_image.h>   /* shell_xsex[], shell_xsex_len: the build's, for this target */

/* Every call goes through the system page: the kernel hands _start its base
   in esi (rsi on x86-64), and the stub for syscall n is SYSPAGE_STUB_SIZE * n
   past it. The stub puts n in eax itself and returns the result there
   (cxk_abi.h). */
static uintptr_t sysbase __attribute__((used));
#define STUB(n) (sysbase + (n) * SYSPAGE_STUB_SIZE)

#if defined(__x86_64__)
/* Arguments in rdi and rsi (CX_SYSCALLS_IPC.md section 4). The stub's
   `syscall` takes rcx and r11, and the kernel gives back every other
   register. The `call` pushes below RSP, so this file is built with
   -mno-red-zone (tools/cmake/CMakeLists.txt). */
static inline int sys_call(uint32_t n, uintptr_t a1, uintptr_t a2) {
    long r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(STUB(n)), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return (int)r;
}
#else
/* Arguments in ebx and ecx. */
static inline int sys_call(uint32_t n, uintptr_t a1, uintptr_t a2) {
    int r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(STUB(n)), "b"(a1), "c"(a2) : "memory");
    return r;
}
#endif

static inline void sys_write(const char *s) { (void)sys_call(SYS_CONSOLE_WRITE, (uintptr_t)s, 0); }
static inline __attribute__((noreturn)) void sys_exit(int code) {
    (void)sys_call(SYS_EXIT, (uintptr_t)code, 0);
    for (;;) { }
}
static inline int sys_ep_create(void) { return sys_call(SYS_EP_CREATE, 0, 0); }
static inline int sys_spawn(struct spawn_args *a) { return sys_call(SYS_SPAWN, (uintptr_t)a, 0); }
static inline int sys_exec_path(const char *p, struct spawn_args *a) {
    return sys_call(SYS_EXEC_PATH, (uintptr_t)p, (uintptr_t)a);
}
static inline int sys_ipc_recv(struct ipc_recv_args *a) { return sys_call(SYS_IPC_RECV, (uintptr_t)a, 0); }
static inline int sys_ipc_reply(struct ipc_reply_args *a) { return sys_call(SYS_IPC_REPLY, (uintptr_t)a, 0); }
static inline void sys_yield(void) { (void)sys_call(SYS_YIELD, 0, 0); }

/* A user address crosses the ABI as a u64 on every target (ABI 12). */
#define UADDR(p) ((uint64_t)(uintptr_t)(p))

static void executive_main(void) __attribute__((used, noreturn));

/* Keep the base before any C runs, since the compiler may use esi/rsi. On
   x86-64 the kernel enters with RSP 16-aligned; a call leaves it 8 past that,
   as executive_main expects of a function it was called into. */
__attribute__((naked)) void _start(void) {
#if defined(__x86_64__)
    __asm__ ("mov %rsi, sysbase(%rip)\n\tand $-16, %rsp\n\tcall executive_main\n\tud2");
#else
    __asm__ ("mov %esi, sysbase\n\tjmp executive_main");
#endif
}

static void executive_main(void) {
    sys_write("CXK executive online (.xoex) - brokering\n");

    int ep = sys_ep_create();
    if (ep < 0) { sys_write("executive: ep_create failed\n"); sys_exit(1); }

    struct spawn_args sa;
    sa.image = UADDR(shell_xsex); sa.image_len = shell_xsex_len;
    sa.name = UADDR("shell");    sa.broker_endpoint = ep;
    sa.caps = GRANT_OS_BASELINE;   /* privileged shell (attenuated to executive's caps) */
    /* No arguments for the shell - but say so rather than leaving the fields
       as whatever was on the stack. The kernel validates args against
       args_len, so a stale pointer here is a spawn that fails with E_BAD_ADDRESS
       and a machine that boots to nothing. */
    sa.args = 0; sa.args_len = 0;
    sa.job = 0;                    /* its own job: the kernel's */
    sa.handles = 0; sa.handle_count = 0;   /* none named: it holds only its broker */
    int shell = sys_spawn(&sa);   /* a process handle; it closes when this exits */
    if (shell < 0) { sys_write("executive: spawn failed\n"); sys_exit(1); }

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
     * SPAWN_BACKGROUND: it and every service it starts print to the kernel
     * log, not the screen, which is the shell's. The shell's `log` reads it.
     *
     * A machine with no supervisor on disk is not a broken machine, it is a
     * machine with no services - so this is a note, not a failure. */
    struct spawn_args svc;
    svc.image = 0; svc.image_len = 0;
    svc.name = UADDR("supervisor"); svc.broker_endpoint = -1;
    svc.caps = GRANT_OS_BASELINE | SPAWN_BACKGROUND;
    svc.args = 0; svc.args_len = 0;
    svc.job = 0;
    svc.handles = 0; svc.handle_count = 0;
    if (sys_exec_path("/System/Programs/supervisor.xsex", &svc) < 0)
        sys_write("executive: no service supervisor (/System/Programs/supervisor.xsex)\n");

    /* serve one request from the app */
    char buf[256];
    struct ipc_msg_info info;
    struct ipc_recv_args ra;
    ra.ep_handle = ep; ra.buf = UADDR(buf); ra.cap = sizeof buf - 1; ra.info = UADDR(&info);
    ra.handles = 0; ra.handle_cap = 0;
    int n = sys_ipc_recv(&ra);          /* blocks until the app ipc_calls */
    if (n >= 0) {
        buf[n] = 0;                      /* NUL-terminate for the write */
        sys_write(buf);                  /* print the app's message FOR it */

        struct ipc_reply_args rp;
        rp.token = info.reply; rp.data = UADDR("ok"); rp.len = 2;
        rp.handles = 0; rp.handle_count = 0;
        sys_ipc_reply(&rp);              /* wake the app */
    }

    sys_yield();                         /* let the app resume and exit */
    sys_write("CXK executive: served the app, shutting down\n");
    sys_exit(0);
}