// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /os/apps/hello/hello.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Tiny CXK USER app (.xuex), capability-less. It cannot touch the console
 * directly (no GRANT_CONSOLE - that was CP2's E_PERM proof). Instead it asks its
 * broker executive to print a message on its behalf, over IPC: it ipc_calls
 * its broker endpoint (handle 0) with the text as the request, blocks until the
 * executive replies, then exits. The text reaching the screen - printed BY the
 * executive - is the positive half of the brokered model.
 */

#include "cxk_abi.h"

static inline long ipc_call(struct ipc_call_args *a) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_IPC_CALL), "b"(a) : "memory");
    return r;
}
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    for (;;) { }
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

void _start(void) {
    const char *msg = "    hello from the app, printed for it by the broker\n";
    char reply[8];
    struct ipc_call_args a;
    a.ep_handle = 0;                 /* the broker channel (installed by spawn) */
    a.req       = msg;
    a.req_len   = slen(msg);
    a.reply     = reply;
    a.reply_cap = sizeof reply;
    long rc = ipc_call(&a);          /* blocks; executive prints msg + replies */
    sys_exit(rc >= 0 ? 0 : 1);
}