/* /CXK/kernel/kconfig.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Kernel configuration: /System/Config/kernel.xkco, an X Data document read
 * once at boot.
 *
 *   kernel_stack_kib = 8      // each kernel stack: 8 to 32 KiB, a multiple of 4
 *   max_threads      = 8      // threads alive at once: 4 to 32
 *   memory_quota_mib = 16     // default SYS_MEM_OP quota per process: 1 to 64 MiB
 *
 * All three are optional; an absent key keeps its built-in default. The file is
 * checked WHOLE before any of it is applied: a syntax error, an unknown key, a
 * value of the wrong kind or out of range refuses the entire file, logs where,
 * and the kernel boots on its built-in defaults. A typo is never half-applied -
 * which matters for exactly the settings a kernel reads before it can ask
 * anyone what was meant.
 *
 * Read with the X Data reader linked in from os/std/xdata.xfxn (xdata.h). The
 * build checks the same file with `cxk check-xdata` before staging it.
 */
#ifndef KCONFIG_H
#define KCONFIG_H

#include <stdint.h>

#define KCONFIG_PATH "/System/Config/kernel.xkco"
#define KCONFIG_CAP  4096        /* largest file read; bigger is refused */

struct kconfig {
    uint32_t kernel_stack;       /* bytes */
    uint32_t max_threads;
    uint32_t memory_quota;       /* bytes */
};

/* the built-in values, used for anything the file does not set */
void kconfig_defaults(struct kconfig *out);

/* Check and read a document. On success returns 0 and fills *out (defaults
   first, then what the file sets). On failure returns nonzero, leaves *out
   untouched, and sets *err_at to the byte offset and *why to a message.
   Touches nothing else, so the self-tests can run it on any text. */
int kconfig_parse(const char *s, uint32_t len, struct kconfig *out,
                  uint32_t *err_at, const char **why);

/* Read KCONFIG_PATH, and apply it or refuse it. Logs either way. Call once,
   after the system volume is mounted and before anything creates a thread. */
void kconfig_load(void);

#endif
