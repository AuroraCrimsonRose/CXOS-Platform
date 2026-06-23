/* /CXK/os/apps/hello/hello.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Tiny CXK USER app (.xcex). Capability-less: when the kernel runs it, caps=0.
 * It tries to write to the console directly - which the kernel DENIES (E_PERM),
 * because only the broker executive holds CAP_CONSOLE. That denial is the whole
 * point: an app cannot reach a privileged primitive; it must go through its
 * executive (IPC, CP3). The app then exits.
 */

#include "cxk_abi.h"

static inline long sys_console_write(const char *s) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_CONSOLE_WRITE), "b"(s), "c"(0) : "memory");
    return r;   /* expected: E_PERM (-1) - the app has no CAP_CONSOLE */
}
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    for (;;) { }
}

void _start(void) {
    /* this should NOT appear on screen: the kernel denies it (E_PERM). */
    long rc = sys_console_write("hi from the app (you should NOT see this)\n");
    /* exit code carries the result so the story is legible even though the app
       cannot print: 0 = denied as expected, 1 = it somehow wrote. */
    sys_exit(rc == E_PERM ? 0 : 1);
}