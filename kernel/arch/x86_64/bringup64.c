// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/bringup64.c */
/* Aurora Tejeda / CATX Systems */
/* Four generic interfaces the x86-64 kernel calls before the code that owns
   them runs here, each the smallest thing that keeps the caller honest:

   - logging.h, on COM1 through early_con. The real logging.c drives the
     console, which needs the framebuffer or VGA text, and the log ring.
   - platform_tick_start, the one part of the PC platform (platform/pc/
     platform.c) the timer needs: the PIT. The rest of platform.c brings the
     PS/2 devices, and with them the mouse and USB HID.
   - keyboard_tick, which wakes a USB keyboard reader; there is none yet.
   - syscall_dispatch (cpu/usermode.h), with four calls: SYS_EXIT, SYS_YIELD,
     SYS_GETPID and SYS_CONSOLE_WRITE, each as usermode.c does it, through
     the recording point. The real table brings the filesystem, IPC, the
     display and the network with it.

   Listed in docs/planning/TRANSITIONAL_CODE.md; each goes when its owner is
   built for x86-64 (step 5f, when the drivers run here). The syscall table
   goes with them: usermode.c joins with kmain. */

#include "early_con.h"
#include "logging.h"
#include "keyboard.h"
#include "platform/platform.h"
#include "platform/pc/pit.h"
#include "usermode.h"
#include "arch/usermode.h"
#include "sysrec.h"
#include "sched.h"
#include "caps.h"
#include "sync/atomic.h"

/* ---- logging.h ---- */

static const char *const sev_name[] = { "INFO", "OK  ", "WARN", "ERR ", "FAIL" };

static int quiet;

static const char *sev_of(log_sev_t sev) {
    return (unsigned)sev < sizeof(sev_name) / sizeof(sev_name[0]) ? sev_name[sev] : sev_name[0];
}

/* klog's child lines indent to its message column: "[" 8 "] - [" 4 "] - " */
static void child_indent(void) {
    for (int i = 0; i < 1 + LOG_TAG_W + 5 + LOG_SEV_W + 4; i++) early_putc(' ');
}

void klog(const char *tag, log_sev_t sev, const char *msg) {
    early_log(tag, sev_of(sev), msg ? msg : "");
}

/* There is no screen yet, so a line meant only for the serial port is a klog. */
void klog_offscreen(const char *tag, log_sev_t sev, const char *msg) {
    klog(tag, sev, msg);
}

void klog_u32(const char *tag, log_sev_t sev,
              const char *label, uint32_t value, uint8_t value_attr,
              const char *suffix) {
    (void)value_attr;
    early_log_start(tag, sev_of(sev));
    if (label) early_puts(label);
    early_dec(value);
    if (suffix) early_puts(suffix);
    early_puts("\n");
}

void klog_child(const char *msg) {
    child_indent();
    early_puts(msg ? msg : "");
    early_puts("\n");
}

void klog_child_u32(const char *label, uint32_t value, uint8_t value_attr,
                    const char *suffix) {
    (void)value_attr;
    child_indent();
    if (label) early_puts(label);
    early_dec(value);
    if (suffix) early_puts(suffix);
    early_puts("\n");
}

void klog_quiet(void) { quiet = 1; }
int  klog_is_quiet(void) { return quiet; }

/* ---- platform/platform.h: the tick ---- */

void platform_tick_start(uint32_t hz, irq_handler_t on_tick) {
    pit_start_periodic(hz, on_tick);
}

/* ---- keyboard.h ---- */

void keyboard_tick(void) { }

/* ---- cpu/usermode.h: the system call table ---- */

static atomic_u32 dispatched;
uint32_t syscall_dispatch_count(void) { return atomic_load(&dispatched); }

extern atomic_u32 syscall_trapped;   /* usermode.asm */
uint32_t syscall_trapped_count(void) { return atomic_load(&syscall_trapped); }

/* SYS_CONSOLE_WRITE onto COM1, a piece at a time: a length (no NUL scan),
   GRANT_CONSOLE, and the buffer readable from ring 3, as usermode.c asks. */
static int console_write(uint32_t buf, uint32_t len) {
    if (!(thread_current_caps() & GRANT_CONSOLE)) return E_PERM;
    if (len == 0 || len > 0x1000) return E_INVAL;
    if (!user_ptr_readable(buf, len)) return -1;
    char piece[64];
    for (uint32_t done = 0; done < len; ) {
        uint32_t n = len - done < sizeof piece ? len - done : (uint32_t)sizeof piece;
        if (user_copy_in(piece, buf + done, n) != 0) return E_FAULT;
        for (uint32_t i = 0; i < n; i++) early_putc(piece[i]);
        done += n;
    }
    return (int)len;
}

static int syscall_do(uint32_t num, uint32_t a1, uint32_t a2) {
    atomic_fetch_add(&dispatched, 1);
    switch (num) {
    case SYS_EXIT:
        return_to_kernel((int)a1, thread_current_usave());
        return 0;   /* unreachable */
    case SYS_YIELD:
        yield();
        return 0;
    case SYS_GETPID:
        return thread_current_pid();
    case SYS_CONSOLE_WRITE:
        return console_write(a1, a2);
    default:
        return E_NOSYS;
    }
}

int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2) {
    sysrec_enter(num, a1, a2);
    int rc = syscall_do(num, a1, a2);
    sysrec_exit(num, rc);
    return rc;
}
