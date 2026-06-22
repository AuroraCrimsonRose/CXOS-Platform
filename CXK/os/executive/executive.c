/* /CXK/os/executive/executive.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK system executive - bootstrap (.xoex), the first ring-3 code the kernel
 * hands the machine to. This C version is a placeholder proving the kernel ->
 * executive handoff; it will be rewritten in X once the language exists. For
 * now it just announces itself from ring 3 and exits cleanly.
 *
 * Syscalls (CXK ABI): eax = number, ebx = arg1, ecx = arg2, int 0x80.
 *   SYS_EXIT(0): ebx = exit code      SYS_WRITE(1): ebx = string, ecx = len (0 = NUL-scan)
 */

#define SYS_EXIT   0
#define SYS_WRITE  1

static inline void sys_write(const char *s) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_WRITE), "b"(s), "c"(0) : "memory");
}
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    for (;;) { }   /* SYS_EXIT does not return to ring 3 */
}

void _start(void) {
    sys_write("CXK executive online: hello from ring 3 (.xoex)\n");
    sys_exit(0);
}