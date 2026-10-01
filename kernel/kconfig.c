/* /kernel/kconfig.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* Kernel configuration - see kconfig.h. */

#include "kconfig.h"
#include "xdata.h"
#include "sched.h"
#include "vmregion.h"
#include "cxfs.h"
#include "logging.h"
#include "color.h"
#include "format.h"

#define KIB 1024u
#define MIB (1024u * 1024u)

void kconfig_defaults(struct kconfig *out) {
    out->kernel_stack = THREAD_STACK_DEFAULT;
    out->max_threads  = THREAD_LIMIT_DEFAULT;
    out->memory_quota = VM_DEFAULT_QUOTA;
}

/* One integer setting: read it, range-check it, or say why not. */
static int setting(const char *s, struct xd_val *v, uint32_t lo, uint32_t hi,
                   uint32_t *out, const char **why, const char *range_msg) {
    uint32_t u;
    int32_t rc = xd_u32(s, v, &u);
    if (rc == XD_E_TYPE)  { *why = "must be a whole number"; return -1; }
    if (rc != XD_OK || u < lo || u > hi) { *why = range_msg; return -1; }
    *out = u;
    return 0;
}

int kconfig_parse(const char *s, uint32_t len, struct kconfig *out,
                  uint32_t *err_at, const char **why) {
    int32_t rc = xd_check(s, len);
    if (rc != XD_OK) { *err_at = xd_err_at; *why = xd_strerror(rc); return rc; }

    /* into a copy, so a refusal leaves the caller's values exactly as they were */
    struct kconfig c;
    kconfig_defaults(&c);

    struct xd_val root, k, v;
    xd_root(s, len, &root);
    for (uint32_t i = 0; xd_entry_at(s, &root, i, &k, &v) == 1; i++) {
        uint32_t n;
        *err_at = v.at;
        if (xd_sym_is(s, &k, "kernel_stack_kib")) {
            if (setting(s, &v, 8, 32, &n, why, "kernel_stack_kib must be 8 to 32") < 0) return -1;
            if (n % 4) { *why = "kernel_stack_kib must be a multiple of 4"; return -1; }
            c.kernel_stack = n * KIB;
        } else if (xd_sym_is(s, &k, "max_threads")) {
            if (setting(s, &v, THREAD_LIMIT_MIN, THREAD_LIMIT_MAX, &n, why,
                        "max_threads must be 4 to 32") < 0) return -1;
            c.max_threads = n;
        } else if (xd_sym_is(s, &k, "memory_quota_mib")) {
            if (setting(s, &v, 1, 64, &n, why, "memory_quota_mib must be 1 to 64") < 0) return -1;
            c.memory_quota = n * MIB;
        } else {
            *err_at = k.at;
            *why = "unknown key";
            return -1;
        }
    }

    *out = c;
    return 0;
}

/* "line N: why" for the log */
static void log_where(const char *s, uint32_t at, const char *why) {
    char line[96];
    uint32_t p = 0;
    const char *pre = "line ";
    while (*pre) line[p++] = *pre++;
    p += (uint32_t)fmt_u32(line + p, xd_line(s, at));
    line[p++] = ':'; line[p++] = ' ';
    while (*why && p < sizeof(line) - 1) line[p++] = *why++;
    line[p] = 0;
    klog_child(line);
}

void kconfig_load(void) {
    static char text[KCONFIG_CAP];

    struct cxfs_entry e;
    if (cxfs_stat_path(KCONFIG_PATH, &e) != 0) {
        klog("KCONFIG", SEV_INFO, "no " KCONFIG_PATH " - built-in defaults");
        return;
    }
    if (e.size > KCONFIG_CAP) {
        klog("KCONFIG", SEV_WARN, KCONFIG_PATH " refused - built-in defaults used");
        klog_child_u32("larger than the limit in bytes: ", KCONFIG_CAP, LOG_COLOR_VALUE, "");
        return;
    }
    int n = cxfs_read_path(KCONFIG_PATH, text, KCONFIG_CAP);
    if (n < 0 || (uint32_t)n != (uint32_t)e.size) {
        klog("KCONFIG", SEV_WARN, KCONFIG_PATH " unreadable - built-in defaults used");
        return;
    }

    struct kconfig c;
    uint32_t at = 0;
    const char *why = "";
    if (kconfig_parse(text, (uint32_t)n, &c, &at, &why) != 0) {
        klog("KCONFIG", SEV_WARN, KCONFIG_PATH " refused - built-in defaults used");
        log_where(text, at, why);
        return;
    }

    if (sched_configure(c.max_threads, c.kernel_stack) != 0) {
        /* only possible if something already made more threads than the new
           limit allows - which is kmain calling this too late */
        klog("KCONFIG", SEV_WARN, "thread limit below threads already running - not applied");
        return;
    }
    vm_set_default_quota(c.memory_quota);

    klog("KCONFIG", SEV_OK, KCONFIG_PATH " applied");
    klog_child_u32("kernel stack KiB: ", c.kernel_stack / KIB, LOG_COLOR_VALUE, "");
    klog_child_u32("max threads: ", c.max_threads, LOG_COLOR_VALUE, "");
    klog_child_u32("memory quota MiB: ", c.memory_quota / MIB, LOG_COLOR_VALUE, "");
}
