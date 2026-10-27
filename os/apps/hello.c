// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /os/apps/hello/hello.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Tiny CXK USER app (.xuex), capability-less. It cannot touch the console
 * directly (no GRANT_CONSOLE - that was CP2's E_ACCESS_DENIED proof). Instead it asks its
 * broker executive to print a message on its behalf, over IPC: it ipc_calls
 * its broker endpoint (the handle SYS_ARGS reports) with the text as the
 * request, blocks until the
 * executive replies, then exits. The text reaching the screen - printed BY the
 * executive - is the positive half of the brokered model.
 */

#include "cxk_abi.h"

/* Calls go through the system page, whose base the kernel hands _start in
   esi; the stub for syscall n is SYSPAGE_STUB_SIZE * n past it (cxk_abi.h). */
static uint32_t sysbase __attribute__((used));
#define STUB(n) (sysbase + (n) * SYSPAGE_STUB_SIZE)

static inline long ipc_call(struct ipc_call_args *a) {
    long r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(STUB(SYS_IPC_CALL)), "b"(a) : "memory");
    return r;
}
static inline long sys_args(struct args_info *ai) {
    long r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(STUB(SYS_ARGS)), "b"(ai) : "memory");
    return r;
}
static inline __attribute__((noreturn)) void sys_exit(int code) {
    long r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(STUB(SYS_EXIT)), "b"(code) : "memory");
    for (;;) { }
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

static void hello_main(void) __attribute__((used, noreturn));

/* Keep the base before any C runs, since the compiler may use esi. */
__attribute__((naked)) void _start(void) {
    __asm__ ("mov %esi, sysbase\n\tjmp hello_main");
}

static void hello_main(void) {
    const char *msg = "    hello from the app, printed for it by the broker\n";
    char reply[8];
    struct args_info ai;
    if (sys_args(&ai) != E_OK || ai.broker < 0) sys_exit(1);
    struct ipc_call_args a;
    a.ep_handle = ai.broker;         /* the broker channel (installed by spawn) */
    a.req       = msg;
    a.req_len   = slen(msg);
    a.reply     = reply;
    a.reply_cap = sizeof reply;
    a.reserved  = 0;                 /* must be 0, or E_INVALID_ARGS */
    long rc = ipc_call(&a);          /* blocks; executive prints msg + replies */
    sys_exit(rc >= 0 ? 0 : 1);
}