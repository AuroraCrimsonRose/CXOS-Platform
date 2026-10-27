// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/usermode.c */
/* Aurora Tejeda / CATX Systems */
/* Ring 3 entry, syscall dispatch, and the user-mode tests (Checkpoints 3a/3b). */

#include "usermode.h"
#include "arch/irq.h"        /* set_user_fault_hook, irq_frame_* */
#include "format.h"          /* fmt_hex, fmt_u32 */
#include "diag/crashlog.h"   /* crashlog_note_fault, crashlog_save_faults */
#include "arch/usermode.h"   /* enter_usermode, return_to_kernel, the gate */
#include "sched.h"
#include "uid.h"
#include "arch/paging.h"
#include "pmm.h"
#include "heap.h"
#include "vmregion.h"
#include "console.h"
#include "logging.h"
#include "syslog.h"           /* slog_get, for SYS_LOG_READ */
#include "proc.h"             /* ->background, proc_out_write */
#include "color.h"
#include "fb.h"
#include "caps.h"
#include "object/htab.h"
#include "sync/atomic.h"
#include "sysrec.h"
#include "systable.h"         /* the handlers, decoders and table (generated) */
#include "spawn.h"
#include "sysfile.h"
#include "cxfs.h"
#include "spawn.h"

#include "ipc.h"
#include "keyboard.h"
#include "mouse.h"
#include "power.h"
#include "netif.h"
#include "timer.h"
#include "platform/platform.h"
#include "datetime.h"


/* ---- v5 user memory --------------------------------------------------------
 * v4 mapped user pages identity (virt==phys) at low physical RAM, ASSUMING that
 * RAM was free. In v5 the pmm owns physical memory, so user pages must be backed
 * by frames ALLOCATED from the pmm and mapped at user-VIRTUAL addresses (low,
 * below the kernel's 0xC0000000, so ring 3 can reach them). The kernel copies
 * the blob in via the user-virtual address - legal because ring 0 may touch
 * user pages, and the mapping is live in the current address space.
 *
 * user virtual layout (per process slot):
 *   code_virt  = R3_BASE + pid * R3_SLOT_SIZE
 *   stack_virt = code_virt + 0x1000
 */
#define R3_BASE       0x00800000u      /* 8 MB: base of user virtual region */
#define R3_SLOT_SIZE  0x00010000u      /* 64 KB virtual slot per process */
/* Slots, so thread ids, it serves: 8 MB to 10 MB, the range these helpers
   have always used (thread ids were below 32 while the limit was). */
#define R3_SLOTS      32
#define USER_MSG_OFF  0x800            /* message sits 2KB into the code page */

/* single-test (3a) user virtual addresses (pid-independent fixed slot 0) */
#define TEST_CODE_VIRT   R3_BASE
#define TEST_STACK_VIRT  (R3_BASE + 0x1000)

/* map one fresh pmm frame at virtual `virt` (a user page). returns the physical
   frame (so it can be freed later), or 0 on failure. */
static paddr_t map_user_page(uint32_t virt) {
    paddr_t phys = pmm_alloc();
    if (!phys) return 0;
    if (paging_map_user(virt, phys, VM_WRITE) != 0) { pmm_free(phys); return 0; }
    return phys;
}

static void unmap_user_page(uint32_t virt, paddr_t phys) {
    paging_unmap(virt);
    if (phys) pmm_free(phys);
}

/* A code page, once written: executable from ring 3 and no longer writable
   (W^X). Mapped VM_WRITE it is no-execute where the MMU has the bit, and its
   first instruction faults (x86-64); x86-32 runs every page, so there this
   only takes the write away. 0, or -1 as paging_map_user. */
static int seal_user_code(uint32_t virt, paddr_t phys) {
    return paging_map_user(virt, phys, VM_EXEC);
}

/* Checking and copying user buffers: usercopy.c. */

/* ---- syscall dispatch ---- */

/* Calls the dispatcher has handled: a witness for the recording point,
   kept here rather than in sysrec.c so the two are counted apart. */
/* object_info reports the kernel's type numbers as they are. */
_Static_assert(KOBJ_ENDPOINT == OBJ_TYPE_ENDPOINT && KOBJ_FILE == OBJ_TYPE_FILE,
               "kobj types must be the ABI's OBJ_TYPE_*");
/* SYS_LOG_READ copies the ring's entries field by field into the ABI's. */
_Static_assert(SLOG_SYS_W == LOG_SYSTEM_W && SLOG_SUB_W == LOG_SYSTEM_W && SLOG_EVT_W == LOG_EVENT_W,
               "the log ring's fields must be the ABI's log_entry widths");

static atomic_u32 dispatched;
uint32_t syscall_dispatch_count(void) { return atomic_load(&dispatched); }

extern atomic_u32 syscall_trapped;   /* usermode.asm */
uint32_t syscall_trapped_count(void) { return atomic_load(&syscall_trapped); }

/* The handlers, one per syscall. Their signatures, the decoders that give
   each its arguments, and the table that finds them are generated from
   abi/kernel.xfil (systable.h, systable.c); what each one does is here. */

int syscall_console_write(uint32_t buf, uint32_t len) {
    if (!(thread_current_caps() & GRANT_CONSOLE)) {
        klog_u32("GRANT", SEV_WARN, "console_write DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_CONSOLE)");
        return E_ACCESS_DENIED;
    }
    if (len == 0) {
        const char *p = (const char *)user_ptr(buf);
        while (len < 0x1000 && user_ptr_readable(buf + len, 1) && p[len]) len++;
    }
    if (!user_ptr_readable(buf, len ? len : 1)) return -1;
    const char *s = (const char *)user_ptr(buf);

    /* A background process (SPAWN_BACKGROUND, the supervisor and its
       services) writes to the log, so it never prints over the shell. */
    struct proc *p = thread_proc(thread_current_id());
    if (p && p->background) { proc_out_write(p, s, len); return (int)len; }
    for (uint32_t i = 0; i < len; i++) console_putc(s[i]);
    return (int)len;
}

int syscall_log_read(uint32_t i, uint32_t e) {
    /* GRANT_CONSOLE: the log holds what every process printed in the
       background, and what the kernel said about all of them. */
    if (!(thread_current_caps() & GRANT_CONSOLE)) return E_ACCESS_DENIED;
    if (!user_ptr_writable(e, sizeof(struct log_entry))) return E_BAD_ADDRESS;
    const struct slog_entry *le = slog_get(i);
    if (!le) return 0;
    struct log_entry *out = (struct log_entry *)user_ptr(e);
    out->seq = le->seq;
    out->sev = le->sev;
    for (int k = 0; k < LOG_SYSTEM_W; k++) out->system[k]    = le->system[k];
    for (int k = 0; k < LOG_SYSTEM_W; k++) out->subsystem[k] = le->subsystem[k];
    for (int k = 0; k < LOG_EVENT_W;  k++) out->event[k]     = le->event[k];
    return 1;
}

int syscall_getpid(void) { return thread_current_pid(); }
int syscall_getuid(void) { return (int)thread_current_uid(); }

void syscall_exit(int32_t code) {
    return_to_kernel((int)code, thread_current_usave());
}

void syscall_yield(void) { yield(); }

int syscall_clock(uint32_t c) {
    /* Unprivileged: the time of day is not authority. A process that
       could not read it would simply count its own loop iterations. */
    if (!user_ptr_writable(c, sizeof(struct clock_info))) return E_BAD_ADDRESS;
    struct clock_info *ci = (struct clock_info *)user_ptr(c);
    ci->ticks = timer_ticks();

    /* epoch 0 means "no usable RTC", which the ABI documents, so a
       caller that needs a real date can tell. Deriving it here rather
       than handing over the clock's raw fields keeps its format (BCD and
       century, on a PC) on this side of the boundary. */
    struct datetime dt;
    ci->epoch = platform_wallclock(&dt) == 0
              ? (uint32_t)datetime_to_epoch(&dt) : 0;
    return E_OK;
}

int syscall_args(uint32_t a) {
    /* Unprivileged: a program asking where its own arguments are.
       This exists so USER_ARGS_BASE does not have to be compiled into
       every program - the address is a 32-bit x86 fact, and a userland
       that knew it would have to be ported alongside the kernel. */
    if (!user_ptr_writable(a, sizeof(struct args_info))) return E_BAD_ADDRESS;
    struct args_info *ai = (struct args_info *)user_ptr(a);
    ai->base  = USER_ARGS_BASE;
    ai->count = *(const uint32_t *)user_ptr(USER_ARGS_BASE);
    ai->broker = thread_broker(thread_current_id());
    return E_OK;
}

int syscall_sleep(uint32_t ms) {
    /* Also unprivileged: waiting is the opposite of a privilege. Note
       this is NOT POWER_SLEEP, which puts the machine into an ACPI
       sleep state and is rightly gated - this blocks one thread and
       leaves everything else running.

       Capped rather than unbounded: a sleep is a promise to come back,
       and one that never does is indistinguishable from a hang. */
    if (ms > SLEEP_MAX_MS) return E_OUT_OF_RANGE;
    /* The tick it woke on is deliberately not returned: SYS_SLEEP's
       ABI result is a status, and a caller that wants the clock asks
       SYS_CLOCK. Cast so this reads as a decision. */
    (void)thread_sleep_ms(ms);
    return E_OK;
}

int syscall_spawn(uint32_t a) {
    if (!(thread_current_caps() & GRANT_SPAWN)) return E_ACCESS_DENIED;
    return sys_spawn(a);
}

int syscall_exec_path(uint32_t path, uint32_t a) {
    if (!(thread_current_caps() & GRANT_SPAWN)) return E_ACCESS_DENIED;
    return sys_exec_path(path, a);
}

int syscall_file_op(uint32_t a) {
    if (!(thread_current_caps() & GRANT_DISK)) {
        klog_u32("GRANT", SEV_WARN, "file_op DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_DISK)");
        return E_ACCESS_DENIED;
    }
    return sys_file_op(a);
}

/* The seven framebuffer calls share one gate, as the power calls do. */
static int fb_granted(void) {
    if (thread_current_caps() & GRANT_FRAMEBUFFER) return 1;
    klog_u32("GRANT", SEV_WARN, "framebuffer DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_FRAMEBUFFER)");
    return 0;
}

int syscall_fb_info(uint32_t g) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_info(g);
}

int syscall_fb_clear(uint32_t color) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_clear(color);
}

int syscall_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_fill_rect(x, y, w, h, color);
}

int syscall_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_put_pixel(x, y, color);
}

int syscall_fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_draw_line(x0, y0, x1, y1, color);
}

int syscall_fb_draw_text(uint32_t x, uint32_t y, uint32_t text, uint32_t fg, uint32_t bg) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_draw_text(x, y, text, fg, bg);
}

int syscall_fb_set_font(uint32_t name) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_set_font(name);
}

/* unprivileged: reading your own keyboard input */
int syscall_input_read(void) {
    return (int)(unsigned char)keyboard_getchar_blocking();
}

int syscall_input_poll(void) {
    return (int)(unsigned char)keyboard_getchar();   /* 0 if none waiting */
}

int syscall_net_op(uint32_t a) {
    if (!(thread_current_caps() & GRANT_NET)) {
        klog_u32("GRANT", SEV_WARN, "net DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_NET)");
        return E_ACCESS_DENIED;
    }
    return sys_net_op(a);
}

int syscall_mouse_read(uint32_t m) {
    /* unprivileged, same reasoning as keyboard input: a process reading
       the pointer it is already being shown */
    struct mouse_state *ms = (struct mouse_state *)user_ptr(m);
    if (!user_ptr_writable(m, sizeof *ms)) return E_BAD_ADDRESS;
    ms->x       = mouse_x();
    ms->y       = mouse_y();
    ms->buttons = mouse_buttons();
    ms->seq     = mouse_seq();
    return mouse_present() ? 1 : 0;
}

/* The three power calls share one gate, so none of them can drift out of it. */
static int power_granted(void) {
    if (thread_current_caps() & GRANT_POWER) return 1;
    klog_u32("GRANT", SEV_WARN, "power DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_POWER)");
    return 0;
}

int syscall_power_reboot(void) {
    if (!power_granted()) return E_ACCESS_DENIED;
    power_reboot();                     /* does not return */
    return E_OK;
}

int syscall_power_shutdown(void) {
    if (!power_granted()) return E_ACCESS_DENIED;
    power_shutdown();                   /* does not return: S5, or halt */
    return E_OK;
}

int syscall_power_sleep(uint32_t ms) {
    if (!power_granted()) return E_ACCESS_DENIED;
    power_sleep(ms);
    return E_OK;
}

/* The memory calls are unprivileged, like the clock and the sleep: a program
   that could not obtain memory would not be contained, it would be unable to
   run. What bounds them is the per-process quota inside vm_map, not a grant
   bit every program would have to hold. The quota is the process's, so each
   passes the caller's pid. */
int syscall_vm_map(uint32_t a) {
    if (!user_ptr_writable(a, sizeof(struct vm_map_args))) return E_BAD_ADDRESS;
    return vm_map(thread_current_pid(), (struct vm_map_args *)user_ptr(a));
}

int syscall_vm_unmap(uint32_t addr, uint32_t length) {
    return vm_unmap(thread_current_pid(), addr, length);
}

int syscall_vm_info(uint32_t u) {
    if (!user_ptr_writable(u, sizeof(struct vm_usage))) return E_BAD_ADDRESS;
    return vm_info(thread_current_pid(), (struct vm_usage *)user_ptr(u));
}

int syscall_ipc_call(uint32_t a)  { return ipc_call(a); }
int syscall_ipc_recv(uint32_t a)  { return ipc_recv(a); }
int syscall_ipc_reply(uint32_t a) { return ipc_reply(a); }

int syscall_ep_create(void) {
    if (!(thread_current_caps() & GRANT_ENDPOINT)) return E_ACCESS_DENIED;
    return ep_create();
}

int syscall_handle_close(int32_t h) {
    return htab_close(thread_htab(thread_current_id()), h);
}

int syscall_handle_duplicate(int32_t h, uint32_t rights) {
    /* Unprivileged: a duplicate is never more authority than its source. */
    return htab_dup(thread_htab(thread_current_id()), h, rights);
}

int syscall_object_info(uint32_t a) {
    /* Unprivileged: it describes only a handle the caller holds. */
    struct object_info_args oa;
    if (user_copy_in(&oa, a, sizeof oa) != 0) return E_BAD_ADDRESS;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), oa.handle,
                              KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    struct object_basic b = {
        .kind = o->type, .rights = hi.rights, .koid = o->koid,
        .grant = hi.grant, .parent_grant = hi.parent,
    };
    kobj_unref(o);
    if (oa.topic != OBJINFO_BASIC) return E_INVALID_ARGS;
    if (oa.cap < sizeof b) return E_BUFFER_TOO_SMALL;
    if (user_copy_out((uint32_t)oa.buf, &b, sizeof b) != 0) return E_BAD_ADDRESS;
    return (int)sizeof b;
}

/* Called from the system-call gate (arch/usermode.h), and by the self-tests,
   with the five argument registers as ring 3 left them. Every call passes the
   recording point on its way in and on its way out (sysrec.h). A number past
   the table, or in one of its holes, is E_NOT_SUPPORTED. */
int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3,
                     uint32_t a4, uint32_t a5) {
    const uint32_t args[SYSCALL_ARGS] = { a1, a2, a3, a4, a5 };
    sysrec_enter(num, args);
    atomic_fetch_add(&dispatched, 1);
    syscall_decoder d = num < SYSCALL_TABLE_SIZE ? syscall_table[num] : 0;
    int rc = d ? d(args) : E_NOT_SUPPORTED;
    sysrec_exit(num, rc);
    return rc;
}

/* the position-independent ring-3 routines (usermode.asm) */
extern uint8_t user_blob_start[];
extern uint8_t user_blob_end[];

/* Ring-3 SYS_WRITE test payload. This is raw USER output (printed byte-for-byte
   by SYS_WRITE in the current console color - a ring-3 program can't set the
   tag/severity colors klog uses), so it's shaped to match a klog_child detail
   line: 19 leading spaces (LOG_PREFIX_W - 3) + "-> " aligns it under the
   message column of the surrounding log. */
static const char user_msg[] = "                   -> ring 3 SYS_WRITE ok\n";

void usermode_init(void) {
    arch_syscall_gate_init();   /* the gate ring 3 calls the kernel through */
    sysfile_init();   /* registers the open-file object type */
    ipc_init();       /* registers the endpoint object type */
}

/* ---- Checkpoint 3a: the minimal single-process ring-3 round-trip ---- */
int usermode_test(void) {
    /* back the code + stack pages with real pmm frames, mapped user-accessible */
    paddr_t code_phys  = map_user_page(TEST_CODE_VIRT);
    paddr_t stack_phys = map_user_page(TEST_STACK_VIRT);
    if (!code_phys || !stack_phys) {
        if (code_phys)  unmap_user_page(TEST_CODE_VIRT, code_phys);
        if (stack_phys) unmap_user_page(TEST_STACK_VIRT, stack_phys);
        return -1;
    }

    /* copy the position-independent user routine into the code page */
    uint32_t len = (uint32_t)(user_blob_end - user_blob_start);
    if (len == 0 || len > USER_MSG_OFF) len = USER_MSG_OFF;
    uint8_t *dst = (uint8_t *)TEST_CODE_VIRT;
    for (uint32_t i = 0; i < len; i++) dst[i] = user_blob_start[i];

    /* place the message just after the routine, in the user-accessible page */
    char *umsg = (char *)user_ptr(TEST_CODE_VIRT + USER_MSG_OFF);
    uint32_t i = 0;
    for (; user_msg[i] && i < 0x100; i++) umsg[i] = user_msg[i];
    umsg[i] = '\0';
    int sealed = seal_user_code(TEST_CODE_VIRT, code_phys) == 0;

    /* seed the message pointer at the top of the user stack ([esp] on entry) */
    uint32_t ustack_top = TEST_STACK_VIRT + 0x1000 - 16;
    *(uint32_t *)user_ptr(ustack_top) = user_addr(umsg);

    /* dev test runs as a SYSTEM ring-3 helper: grant it console so the gated
       SYS_WRITE works (restored after). */
    int      tme  = thread_current_id();
    uint32_t tsav = thread_current_caps();
    thread_set_caps(tme, GRANT_CONSOLE);
    /* enter ring 3; returns when the routine SYS_EXITs */
    int rc = sealed ? enter_usermode(TEST_CODE_VIRT, ustack_top, thread_current_usave()) : -1;
    thread_set_caps(tme, tsav);

    /* reclaim the user pages */
    unmap_user_page(TEST_CODE_VIRT, code_phys);
    unmap_user_page(TEST_STACK_VIRT, stack_phys);
    return rc;
}

int usermode_run_blob(const uint8_t *start, uint32_t len) {
    if (!start || len == 0 || len > USER_MSG_OFF) return -1;
    paddr_t code_phys  = map_user_page(TEST_CODE_VIRT);
    paddr_t stack_phys = map_user_page(TEST_STACK_VIRT);
    if (!code_phys || !stack_phys) {
        if (code_phys)  unmap_user_page(TEST_CODE_VIRT, code_phys);
        if (stack_phys) unmap_user_page(TEST_STACK_VIRT, stack_phys);
        return -1;
    }
    uint8_t *dst = (uint8_t *)TEST_CODE_VIRT;
    for (uint32_t i = 0; i < len; i++) dst[i] = start[i];

    int rc = seal_user_code(TEST_CODE_VIRT, code_phys) != 0 ? -1
           : enter_usermode(TEST_CODE_VIRT, TEST_STACK_VIRT + 0x1000 - 16, thread_current_usave());

    unmap_user_page(TEST_CODE_VIRT, code_phys);
    unmap_user_page(TEST_STACK_VIRT, stack_phys);
    return rc;
}


/* ---- the background console and the log -----------------------------------
 * A SPAWN_BACKGROUND process's console writes become log entries tagged with
 * its pid, a line at a time, and leave the screen alone; SYS_LOG_READ hands
 * the ring back. Driven through syscall_dispatch over a user mapping, from
 * this thread's own process with its background flag set for the duration.
 * 1 = pass, 0 = fail. */
static int lt_streq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

int usermode_log_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;

    int me = thread_current_id();
    struct proc *p = thread_proc(me);
    uint32_t save = thread_current_caps();
    int was_bg = p->background;
    thread_set_caps(me, GRANT_CONSOLE);

    char *line = (char *)TEST_CODE_VIRT;
    struct log_entry *out = (struct log_entry *)(TEST_CODE_VIRT + 0x100);
    char tag[12] = "pid ";
    tag[4 + fmt_u32(tag + 4, (uint32_t)p->pid)] = 0;
    int ok = 0, step = 0, col, row, col2, row2;

    /* 1-3: a whole line becomes exactly one entry, and the cursor stays put */
    p->background = 1;
    const char *msg = "bg line\nhalf";
    for (int i = 0; ; i++) { line[i] = msg[i]; if (!msg[i]) break; }
    uint32_t t0 = slog_total();
    console_cursor(&col, &row);
    step = 1; if (syscall_dispatch(SYS_CONSOLE_WRITE, user_addr(line), 0, 0, 0, 0) != 12) goto done;
    console_cursor(&col2, &row2);
    step = 2; if (col2 != col || row2 != row) goto done;
    step = 3; if (slog_total() != t0 + 1) goto done;
    const struct slog_entry *e = slog_get(slog_count() - 1);
    step = 4; if (!lt_streq(e->system, tag, SLOG_SYS_W) || !lt_streq(e->event, "bg line", SLOG_EVT_W)) goto done;

    /* 5: what is left without a newline waits, and a flush records it */
    proc_out_flush(p);
    e = slog_get(slog_count() - 1);
    step = 5; if (slog_total() != t0 + 2 || !lt_streq(e->event, "half", SLOG_EVT_W)) goto done;

    /* 6: a child inherits it, asked for or not */
    struct proc *c = proc_create(p);
    int inherited = c && c->background;
    if (c) proc_put(c);
    step = 6; if (!inherited) goto done;

    /* 7-9: SYS_LOG_READ returns the newest entry, then 0, and needs the grant */
    uint32_t last = slog_count() - 1;
    step = 7; if (syscall_dispatch(SYS_LOG_READ, last, user_addr(out), 0, 0, 0) != 1) goto done;
    step = 8; if (out->seq != e->seq || !lt_streq(out->event, "half", LOG_EVENT_W)
                  || !lt_streq(out->system, tag, LOG_SYSTEM_W)) goto done;
    step = 9; if (syscall_dispatch(SYS_LOG_READ, last + 1, user_addr(out), 0, 0, 0) != 0) goto done;
    thread_set_caps(me, 0);
    step = 10; if (syscall_dispatch(SYS_LOG_READ, last, user_addr(out), 0, 0, 0) != E_ACCESS_DENIED) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "log test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    p->outlen = 0;
    p->background = was_bg;
    thread_set_caps(me, save);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- file syscall test -----------------------------------------------------
 * Drives SYS_FILE_OP the way ring 3 does, short of the int 0x80 gate itself
 * (which the other syscalls already prove). The point of doing it from here
 * rather than from ktest.c is map_user_page: the args and buffers have to live
 * in a user mapping or user_ptr_readable rejects them, which is exactly the
 * check that would otherwise go untested until a real program tripped it.
 *
 * Goes through syscall_dispatch rather than calling sys_file_op directly, so
 * the GRANT_DISK gate is on the path too.
 */
#define FT_ARGS  (TEST_CODE_VIRT + 0x000)
#define FT_PATH  (TEST_CODE_VIRT + 0x100)
#define FT_PATH2 (TEST_CODE_VIRT + 0x180)
#define FT_DATA  (TEST_CODE_VIRT + 0x200)
#define FT_STAT  (TEST_CODE_VIRT + 0x400)
#define FT_INFO  (TEST_CODE_VIRT + 0x600)
#define FT_IARGS (TEST_CODE_VIRT + 0x680)

/* SYS_OBJECT_INFO on `h`, its answer at FT_INFO. */
static int oi_call(int h, uint32_t topic, uint32_t cap) {
    struct object_info_args *a = (struct object_info_args *)FT_IARGS;
    a->handle = h;
    a->topic  = topic;
    a->buf    = FT_INFO;
    a->cap    = cap;
    return syscall_dispatch(SYS_OBJECT_INFO, FT_IARGS, 0, 0, 0, 0);
}

static void ft_str(uint32_t at, const char *s) {
    char *d = (char *)user_ptr(at);
    int i = 0;
    while (s[i] && i < 0x7F) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

/* Scratch-relative paths. The tests write under /Temp rather than littering
   the root, but a volume formatted before the system tree existed has no
   /Temp, so the prefix is chosen at run time and is "" on such a volume. A
   test that only passes on a freshly built image is not much of a test.

   Both helpers build "<prefix><leaf>": ft_strp into the user page for the
   syscall path, ft_join into a kernel buffer for the cxfs_* checks. ft_join
   returns its one static buffer, so use it once per statement. */
static void ft_strp(uint32_t at, const char *pre, const char *leaf) {
    char *d = (char *)user_ptr(at);
    int i = 0;
    while (*pre  && i < 0x7F) d[i++] = *pre++;
    while (*leaf && i < 0x7F) d[i++] = *leaf++;
    d[i] = '\0';
}

static char ft_abs[128];
static const char *ft_join(const char *pre, const char *leaf) {
    int i = 0;
    while (*pre  && i < (int)sizeof ft_abs - 1) ft_abs[i++] = *pre++;
    while (*leaf && i < (int)sizeof ft_abs - 1) ft_abs[i++] = *leaf++;
    ft_abs[i] = '\0';
    return ft_abs;
}

static int ft_call(uint32_t op, int handle, uint32_t path, uint32_t data,
                   uint32_t len, int32_t off, uint32_t flags) {
    struct file_op_args *a = (struct file_op_args *)FT_ARGS;
    a->op     = op;
    a->handle = handle;
    a->path   = path;
    a->path2  = FT_PATH2;
    a->data   = data;
    a->len    = len;
    a->off    = off;
    a->flags  = flags;
    return syscall_dispatch(SYS_FILE_OP, FT_ARGS, 0, 0, 0, 0);
}

int usermode_file_test(void) {
    if (!cxfs_is_mounted()) return 1;         /* nothing mounted - skip */

    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;

    int      me   = thread_current_id();
    uint32_t save = thread_current_caps();
    thread_set_caps(me, GRANT_DISK);

    int ok = 0;       /* set to 1 only at the very end */
    /* Which check failed, counted in execution order. One pass/fail for forty
       assertions is not enough to act on - this is what turned "the file
       syscall test failed" into "rename(\".\") renamed the working directory",
       which is a real bug and not a bad assertion. */
    int step = 0;
    int base = sysfile_open_count();
    char *data = (char *)FT_DATA;

    /* /Temp when the system tree is present, the root when it is not. */
    const char *scratch = (cxfs_resolve("/Temp", 0) >= 0) ? "/Temp" : "";

    /* --- create, write, read back through one handle --- */
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    int h = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0,
                    FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
    step++;
    if (h < 0) goto done;
    step++;
    if (sysfile_open_count() != base + 1) goto done;

    ft_str(FT_DATA, "hello world");
    step++;
    if (ft_call(FILE_OP_WRITE, h, 0, FT_DATA, 11, 0, 0) != 11) goto done;
    step++;
    if (ft_call(FILE_OP_TELL,  h, 0, 0, 0, 0, 0) != 11) goto done;

    /* --- a duplicate narrowed to read cannot write, though the file was
       opened for both: the handle's rights decide, not the open flags. It
       shares the open file (and so the offset), and closing it leaves the
       file to the handle it came from. --- */
    int d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)h, FOPEN_READ, 0, 0, 0);
    step++;
    if (d < 0 || d == h) goto done;
    step++;
    if (syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)d, FOPEN_READ | FOPEN_WRITE, 0, 0, 0) != E_ACCESS_DENIED) goto done;

    /* object_info: d names the same open file as h, with only the rights it
       was given, derived from h's grant; and d, given no duplicate right,
       cannot be copied at all. */
    const int ob = (int)sizeof(struct object_basic);
    step++;
    if (oi_call(h, OBJINFO_BASIC, (uint32_t)ob) != ob) goto done;
    struct object_basic bh = *(struct object_basic *)FT_INFO;
    step++;
    if (oi_call(d, OBJINFO_BASIC, (uint32_t)ob) != ob) goto done;
    struct object_basic bd = *(struct object_basic *)FT_INFO;
    step++;
    if (bh.kind != OBJ_TYPE_FILE || bh.parent_grant != 0 || bh.koid == 0
        || bh.rights != (FOPEN_READ | FOPEN_WRITE | HRIGHT_DUPLICATE)) goto done;
    step++;
    if (bd.kind != OBJ_TYPE_FILE || bd.koid != bh.koid || bd.rights != FOPEN_READ
        || bd.parent_grant != bh.grant || bd.grant == bh.grant) goto done;
    step++;
    if (syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)d, FOPEN_READ, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (oi_call(d, 99, (uint32_t)ob) != E_INVALID_ARGS) goto done;
    step++;
    if (oi_call(d, OBJINFO_BASIC, (uint32_t)ob - 1) != E_BUFFER_TOO_SMALL) goto done;
    step++;
    if (ft_call(FILE_OP_WRITE, d, 0, FT_DATA, 1, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (ft_call(FILE_OP_TRUNC, d, 0, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (ft_call(FILE_OP_TELL, d, 0, 0, 0, 0, 0) != 11) goto done;
    step++;
    if (syscall_dispatch(SYS_HANDLE_CLOSE, (uint32_t)d, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (sysfile_open_count() != base + 1) goto done;  /* h still holds it */
    step++;
    if (ft_call(FILE_OP_TELL, d, 0, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    step++;
    if (oi_call(d, OBJINFO_BASIC, sizeof(struct object_basic)) != E_BAD_HANDLE) goto done;
    step++;
    if (ft_call(FILE_OP_TELL, h, 0, 0, 0, 0, 0) != 11) goto done;

    step++;
    if (ft_call(FILE_OP_SEEK, h, 0, 0, 0, 0, FSEEK_SET) != 0) goto done;
    for (int i = 0; i < 16; i++) data[i] = '?';
    step++;
    if (ft_call(FILE_OP_READ, h, 0, FT_DATA, 11, 0, 0) != 11) goto done;
    step++;
    if (data[0] != 'h' || data[4] != 'o' || data[6] != 'w' || data[10] != 'd') goto done;

    /* seek from the end, and read the tail */
    step++;
    if (ft_call(FILE_OP_SEEK, h, 0, 0, 0, -5, FSEEK_END) != 6) goto done;
    step++;
    if (ft_call(FILE_OP_READ, h, 0, FT_DATA, 5, 0, 0) != 5) goto done;
    step++;
    if (data[0] != 'w' || data[4] != 'd') goto done;
    /* and the read past the end that follows it comes back empty, not short */
    step++;
    if (ft_call(FILE_OP_READ, h, 0, FT_DATA, 5, 0, 0) != 0) goto done;

    /* fstat agrees about the size */
    step++;
    if (ft_call(FILE_OP_FSTAT, h, 0, FT_STAT, 0, 0, 0) != E_OK) goto done;
    struct file_stat *st = (struct file_stat *)FT_STAT;
    step++;
    if (st->size != 11 || st->kind != FTYPE_FILE) goto done;

    step++;
    if (ft_call(FILE_OP_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (sysfile_open_count() != base) goto done;      /* the slot came back */
    step++;
    if (ft_call(FILE_OP_READ, h, 0, FT_DATA, 4, 0, 0) != E_BAD_HANDLE) goto done;

    /* --- append opens at the end, and does not clobber --- */
    h = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_APPEND);
    step++;
    if (h < 0) goto done;
    ft_str(FT_DATA, "!!");
    step++;
    if (ft_call(FILE_OP_WRITE, h, 0, FT_DATA, 2, 0, 0) != 2) goto done;
    step++;
    if (ft_call(FILE_OP_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;

    h = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;
    if (h < 0) goto done;
    step++;
    if (ft_call(FILE_OP_READ, h, 0, FT_DATA, 32, 0, 0) != 13) goto done;
    step++;
    if (data[10] != 'd' || data[11] != '!' || data[12] != '!') goto done;
    /* a read-only handle must refuse a write */
    step++;
    if (ft_call(FILE_OP_WRITE, h, 0, FT_DATA, 2, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (ft_call(FILE_OP_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;

    /* --- a missing file is E_NOT_FOUND without FOPEN_CREATE --- */
    ft_strp(FT_PATH, scratch, "/kt_absent.txt");
    step++;
    if (ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ) != E_NOT_FOUND) goto done;

    /* --- a bad user pointer is caught, not dereferenced --- */
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (ft_call(FILE_OP_STAT, 0, FT_PATH, 0xC0001000u, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (ft_call(FILE_OP_OPEN, 0, 0xC0001000u, 0, 0, 0, FOPEN_READ) != E_BAD_ADDRESS) goto done;

    /* --- directories, cwd, and relative paths --- */
    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (ft_call(FILE_OP_MKDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(FILE_OP_MKDIR, 0, FT_PATH, 0, 0, 0, 0) != E_ALREADY_EXISTS) goto done;
    step++;
    if (ft_call(FILE_OP_CHDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    /* Compare the whole path against what we chdir'd to, terminator included,
       rather than spot-checking a few indices: the index checks silently
       stopped meaning anything the moment the scratch directory moved. */
    const char *want = ft_join(scratch, "/kt_dir");
    int wantlen = 0;
    while (want[wantlen]) wantlen++;
    step++;
    if (ft_call(FILE_OP_GETCWD, 0, 0, FT_DATA, 64, 0, 0) != wantlen) goto done;
    step++;
    for (int i = 0; i <= wantlen; i++) if (data[i] != want[i]) goto done;

    /* a bare name now resolves inside the new cwd, not at the root */
    ft_str(FT_PATH, "inner.txt");
    h = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0,
                FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
    step++;
    if (h < 0) goto done;
    step++;
    if (ft_call(FILE_OP_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (cxfs_resolve(ft_join(scratch, "/kt_dir/inner.txt"), 0) < 0) goto done;   /* it really landed there */

    /* readdir finds it, and stops rather than repeating past the end */
    ft_str(FT_PATH, ".");
    step++;
    if (ft_call(FILE_OP_READDIR, 0, FT_PATH, FT_STAT, 0, 0, 0) != 1) goto done;
    step++;
    if (st->name[0] != 'i' || st->kind != FTYPE_FILE) goto done;
    step++;
    if (ft_call(FILE_OP_READDIR, 0, FT_PATH, FT_STAT, 1, 0, 0) != 0) goto done;

    /* rename, then a non-empty directory refuses to be removed */
    ft_str(FT_PATH2, "renamed.txt");
    step++;
    if (ft_call(FILE_OP_RENAME, 0, FT_PATH, 0, 0, 0, 0) == E_OK) goto done;  /* "." is not renameable */
    ft_str(FT_PATH, "inner.txt");
    step++;
    if (ft_call(FILE_OP_RENAME, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (cxfs_resolve(ft_join(scratch, "/kt_dir/renamed.txt"), 0) < 0) goto done;

    ft_str(FT_PATH, "/");
    step++;
    if (ft_call(FILE_OP_CHDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (ft_call(FILE_OP_UNLINK, 0, FT_PATH, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;  /* not empty */

    /* --- GRANT_DISK really is the gate --- */
    thread_set_caps(me, 0);
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (ft_call(FILE_OP_STAT, 0, FT_PATH, FT_STAT, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    thread_set_caps(me, GRANT_DISK);

    /* --- exec_path refuses everything it should ---
     * The accept path needs a genuinely signed CXEX, which only exists in a
     * SIGN=ON build, so what is checked here is every way it must say no. That
     * is the half that matters: a verifier that never refuses is not one.
     */
    thread_set_caps(me, GRANT_DISK | GRANT_SPAWN);
    struct spawn_args *sa = (struct spawn_args *)FT_STAT;   /* reuse the page */
    sa->image = 0; sa->image_len = 0; sa->name = FT_PATH;
    sa->broker_endpoint = -1; sa->caps = 0;
    sa->args = 0; sa->args_len = 0;

    ft_strp(FT_PATH, scratch, "/kt_absent.xuex");
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_NOT_FOUND) goto done;

    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_IS_DIR) goto done;

    /* a real file whose contents are not a CXEX at all: the signature check
       must refuse it rather than the loader trying to run the bytes */
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_ACCESS_DENIED) goto done;

    /* a bad path pointer is caught, not dereferenced */
    step++;
    if (sys_exec_path(0xC0001000u, user_addr(sa)) != E_BAD_ADDRESS) goto done;
    step++;
    if (sys_exec_path(FT_PATH, 0xC0001000u) != E_BAD_ADDRESS) goto done;

    /* --- clock and sleep, through the real dispatcher ---
     * Both are unprivileged, so they must work with caps stripped to nothing -
     * that is the assertion, not an afterthought. A future edit that gates
     * either one behind a capability breaks here rather than in whatever
     * service was relying on being able to wait. */
    thread_set_caps(me, 0);
    step++;
    if (syscall_dispatch(SYS_CLOCK, FT_DATA, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (((uint32_t *)FT_DATA)[0] == 0) goto done;     /* ticks must have advanced */
    step++;
    if (syscall_dispatch(SYS_SLEEP, 1, 0, 0, 0, 0) != E_OK) goto done;

    /* --- the authorisation matrix, with caps stripped to nothing ---
     *
     * The 2026-10-09 review asked for an audit of authorisation as distinct
     * from pointer validity - "a valid user pointer alone does not authorize
     * the requested operation". The audit found every privileged syscall
     * already gated; this is what keeps that true, because the failure mode is
     * a syscall ADDED later without a gate, which no existing test would
     * notice.
     *
     * Each of these must refuse on the capability alone, before it looks at
     * its argument - so the pointers passed are deliberately garbage. A gate
     * that ran after argument validation would return E_BAD_ADDRESS here and fail
     * this test, which is the ordering worth pinning: authorisation first.
     */
    step++;
    if (syscall_dispatch(SYS_CONSOLE_WRITE, 0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_OP,       0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_INFO,       0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_CLEAR,      0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_FILL_RECT,  0, 0, 1, 1, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_PUT_PIXEL,  0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_DRAW_LINE,  0, 0, 1, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_DRAW_TEXT,  0, 0, 0xC0001000u, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_SET_FONT,   0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_OP,        0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_POWER_REBOOT,   0, 0, 0, 0, 0)          != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_POWER_SHUTDOWN, 0, 0, 0, 0, 0)          != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_POWER_SLEEP,    1, 0, 0, 0, 0)          != E_ACCESS_DENIED) goto done;   /* 1 ms: 0 would idle until a key if the gate were missing */
    step++;
    if (syscall_dispatch(SYS_EP_CREATE,     0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_SPAWN,         0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_EXEC_PATH,     0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;

    /* The SYS_VM_* calls are NOT in that list, and that is the audit's one real
       finding rather than an omission here. They are unprivileged by design -
       a program that cannot obtain memory is not contained, it is unable to
       run - and the per-process quota is what bounds it. GRANT_MEM exists,
       sits in GRANT_OS_BASELINE and is accepted by the supervisor's descriptor
       parser, and gates nothing; it was documented as "map / unmap / sbrk" as
       though it did. Now marked reserved in caps.h.

       Asserted in the positive direction, so that if anyone later gates
       one of them they break here and have to revisit the decision rather than
       discovering it through a service that can no longer allocate.

       "Not E_ACCESS_DENIED" rather than "E_OK": this runs on a kernel thread, which has
       no vm_proc record, so each answers E_INVALID_ARGS. The property under test
       is that none is refused on CAPABILITY grounds, and E_INVALID_ARGS says
       exactly that - the call got past authorisation and failed on its own
       terms. */
    step++;
    if (syscall_dispatch(SYS_VM_MAP,   FT_DATA, 0, 0, 0, 0) == E_ACCESS_DENIED) goto done;
    if (syscall_dispatch(SYS_VM_UNMAP, 0, 0, 0, 0, 0)       == E_ACCESS_DENIED) goto done;
    if (syscall_dispatch(SYS_VM_INFO,  FT_DATA, 0, 0, 0, 0) == E_ACCESS_DENIED) goto done;

    thread_set_caps(me, GRANT_DISK | GRANT_SPAWN);

    /* a bad clock pointer is caught, not written through */
    step++;
    if (syscall_dispatch(SYS_CLOCK, 0xC0001000u, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    /* and a sleep longer than the cap is refused rather than silently clamped:
       a caller that asked for a day and got an hour would never know */
    step++;
    if (syscall_dispatch(SYS_SLEEP, SLEEP_MAX_MS + 1, 0, 0, 0, 0) != E_OUT_OF_RANGE) goto done;

    /* --- argument blobs the kernel must refuse ---
     * check_args runs before anything touches the disk, so the path here is
     * irrelevant - each of these must fail on the blob alone.
     *
     * The unterminated case is the one worth having: build_args_page counts
     * arguments by counting terminators, so a blob whose last byte is not one
     * would leave the final string running off the end of what was copied.
     * The check that stops it is one line, and nothing else would catch it. */
    sa->args = 0xC0001000u; sa->args_len = 8;
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_BAD_ADDRESS) goto done;

    ft_str(FT_DATA, "noterm");
    sa->args = FT_DATA; sa->args_len = 6;   /* stops before the NUL */
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_INVALID_ARGS) goto done;

    sa->args_len = USER_ARGS_MAX + 1;                     /* wider than the page */
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_OUT_OF_RANGE) goto done;

    sa->args = 0; sa->args_len = 4;                       /* a length with no blob */
    step++;
    if (sys_exec_path(FT_PATH, user_addr(sa)) != E_INVALID_ARGS) goto done;

    sa->args = 0; sa->args_len = 0;

    /* and GRANT_SPAWN is the gate: the dispatcher checks it, so go through it */
    thread_set_caps(me, GRANT_DISK);
    step++;
    if (syscall_dispatch(SYS_EXEC_PATH, FT_PATH, user_addr(sa), 0, 0, 0) != E_ACCESS_DENIED) goto done;
    thread_set_caps(me, GRANT_DISK);

    /* --- the reaper releases handles a process never closed --- */
    h = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;
    if (h < 0) goto done;
    step++;
    if (sysfile_open_count() != base + 1) goto done;
    htab_close_all(thread_htab(me));
    step++;
    if (sysfile_open_count() != base) goto done;

    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "file syscall test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* tidy up whatever got made, so a re-run starts from the same state */
    thread_set_caps(me, GRANT_DISK);
    htab_close_all(thread_htab(me));
    int id;
    if ((id = cxfs_resolve(ft_join(scratch, "/kt_dir/renamed.txt"), 0)) >= 0) cxfs_delete_entry((uint32_t)id);
    if ((id = cxfs_resolve(ft_join(scratch, "/kt_dir/inner.txt"), 0))   >= 0) cxfs_delete_entry((uint32_t)id);
    if ((id = cxfs_resolve(ft_join(scratch, "/kt_dir"), 0))             >= 0) cxfs_delete_entry((uint32_t)id);
    if ((id = cxfs_resolve(ft_join(scratch, "/kt_sys.txt"), 0))         >= 0) cxfs_delete_entry((uint32_t)id);
    thread_set_cwd(me, 0);
    thread_set_caps(me, save);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- the memory calls, through the dispatcher ------------------------------
 * ktest.c's memory tests call vm_map() and its neighbours directly, so they
 * say nothing about the syscalls in front of them: a SYS_VM_MAP that refused
 * every pointer booted green. This drives all three through syscall_dispatch
 * with their arguments in a user page. The test thread's process has no
 * account of its own, so it is given one for the length of the test.
 */
#define VT_ARGS  (TEST_CODE_VIRT + 0x000)
#define VT_USAGE (TEST_CODE_VIRT + 0x100)

int usermode_vm_test(void) {
    int pid = thread_current_pid();
    if (vm_quota_of(pid)) return 0;      /* an account here would be clobbered */

    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    vm_proc_init(pid, vm_default_quota(), -1);

    int ok = 0, step = 0;
    uint32_t addr = 0;
    struct vm_map_args *a = (struct vm_map_args *)user_ptr(VT_ARGS);
    struct vm_usage    *u = (struct vm_usage *)user_ptr(VT_USAGE);

    step++;
    if (vm_quota_of(pid) != vm_default_quota()) goto done;

    for (uint32_t i = 0; i < sizeof *a / 4u; i++) ((uint32_t *)a)[i] = 0;
    a->object = MOBJ_ANON;
    a->length = 1;
    a->prot   = MPROT_READ | MPROT_WRITE;
    step++;
    if (syscall_dispatch(SYS_VM_MAP, VT_ARGS, 0, 0, 0, 0) != E_OK) goto done;
    addr = a->addr;
    step++;
    if (!addr || a->granted != PAGE_SIZE) goto done;
    step++;
    if (!user_ptr_writable(addr, PAGE_SIZE)) goto done;

    step++;
    if (syscall_dispatch(SYS_VM_INFO, VT_USAGE, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (u->mapped != PAGE_SIZE || u->quota != vm_default_quota()) goto done;

    /* addr and length arrive in that order: a length that disagrees with the
       mapping is refused, and the right one releases it */
    step++;
    if (syscall_dispatch(SYS_VM_UNMAP, addr, 2u * PAGE_SIZE, 0, 0, 0) != E_INVALID_ARGS) goto done;
    step++;
    if (syscall_dispatch(SYS_VM_UNMAP, addr, PAGE_SIZE, 0, 0, 0) != E_OK) goto done;
    addr = 0;
    step++;
    if (syscall_dispatch(SYS_VM_INFO, VT_USAGE, 0, 0, 0, 0) != E_OK || u->mapped != 0) goto done;

    /* a pointer into the kernel is refused, not written through */
    step++;
    if (syscall_dispatch(SYS_VM_MAP,  0xC0001000u, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (syscall_dispatch(SYS_VM_INFO, 0xC0001000u, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;

    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "memory syscall test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (addr) vm_unmap(pid, addr, 0);
    vm_proc_reset(pid);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- the input and power calls, through the dispatcher ---------------------
 * Keys are injected into the kernel's buffer, so the blocking read has one
 * waiting and returns at once. The input calls run with no grants at all,
 * which is their contract today (the input hole, until the input service
 * replaces them). Reboot and shutdown cannot be run from here; their gate is in
 * the authorisation matrix, with SYS_POWER_SLEEP's. Sleep is run for real.
 */
int usermode_power_input_test(void) {
    int me = thread_current_id();
    uint32_t save = thread_current_caps();
    int ok = 0, step = 0;

    thread_set_caps(me, 0);
    for (int i = 0; i < 256 && syscall_dispatch(SYS_INPUT_POLL, 0, 0, 0, 0, 0) != 0; i++) { }

    /* poll: nothing waiting is 0, then a key, then 0 again: one key, read once */
    step++;
    if (syscall_dispatch(SYS_INPUT_POLL, 0, 0, 0, 0, 0) != 0) goto done;
    keyboard_inject('q');
    step++;
    if (syscall_dispatch(SYS_INPUT_POLL, 0, 0, 0, 0, 0) != 'q') goto done;
    step++;
    if (syscall_dispatch(SYS_INPUT_POLL, 0, 0, 0, 0, 0) != 0) goto done;

    /* a special key is above 0x7F and comes back as itself, not sign-extended */
    keyboard_inject(KEY_UP);
    step++;
    if (syscall_dispatch(SYS_INPUT_POLL, 0, 0, 0, 0, 0) != KEY_UP) goto done;

    /* read: the waiting key, without blocking, and it is consumed */
    keyboard_inject('z');
    step++;
    if (syscall_dispatch(SYS_INPUT_READ, 0, 0, 0, 0, 0) != 'z') goto done;
    step++;
    if (syscall_dispatch(SYS_INPUT_POLL, 0, 0, 0, 0, 0) != 0) goto done;

    /* sleep idles for at least what was asked (no key is waiting to cut it short) */
    thread_set_caps(me, GRANT_POWER);
    uint32_t t0 = timer_ticks();
    step++;
    if (syscall_dispatch(SYS_POWER_SLEEP, 5, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (timer_ticks() - t0 < 5) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "power and input syscall test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    thread_set_caps(me, save);
    return ok;
}

/* ---- the framebuffer calls, through the dispatcher -------------------------
 * Each call gets arguments that tell their order apart (x != y, w != h, a
 * colour whose three channels differ), and what it drew is read back from the
 * screen. The drawing lands in the bottom-right corner, saved first and put
 * back after. SYS_FB_CLEAR's success path is not taken: it would wipe the boot
 * log off the screen. Its gate is in the authorisation matrix with the other
 * six, and SYS_FB_SET_FONT's name rules are the console font test's. With no
 * framebuffer every call answers E_NOT_FOUND, and that is asserted instead.
 */
#define FBT_W    64
#define FBT_H    32
#define FBT_GEOM (TEST_CODE_VIRT + 0x000)
#define FBT_TEXT (TEST_CODE_VIRT + 0x100)
#define FBT_NAME (TEST_CODE_VIRT + 0x200)

static uint32_t fbt_saved[FBT_H][FBT_W];

int usermode_fb_test(void) {
    int me = thread_current_id();
    uint32_t save = thread_current_caps();
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;

    int ok = 0, step = 0, saved = 0;
    uint32_t x0 = 0, y0 = 0;
    char *text = (char *)user_ptr(FBT_TEXT);
    char *name = (char *)user_ptr(FBT_NAME);
    text[0] = 'a'; text[1] = 'b'; text[2] = 'c'; text[3] = '\0';
    name[0] = 'a'; name[1] = '/'; name[2] = 'b'; name[3] = '\0';
    thread_set_caps(me, GRANT_FRAMEBUFFER);

    if (!fb_active()) {
        step++;
        if (syscall_dispatch(SYS_FB_INFO, FBT_GEOM, 0, 0, 0, 0) != E_NOT_FOUND) goto done;
        step++;
        if (syscall_dispatch(SYS_FB_PUT_PIXEL, 0, 0, 0, 0, 0) != E_NOT_FOUND) goto done;
        step++;
        if (syscall_dispatch(SYS_FB_DRAW_TEXT, 0, 0, FBT_TEXT, 0, 0) != E_NOT_FOUND) goto done;
        ok = 1;
        goto done;
    }

    /* geometry, into a user structure; a kernel address is refused */
    struct fb_geometry *g = (struct fb_geometry *)user_ptr(FBT_GEOM);
    step++;
    if (syscall_dispatch(SYS_FB_INFO, FBT_GEOM, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (g->width != fb_width() || g->height != fb_height() ||
        g->bpp != fb_bpp() || g->pitch != fb_pitch()) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_INFO, 0xC0001000u, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;

    step++;
    if (fb_width() < FBT_W || fb_height() < FBT_H) goto done;
    x0 = fb_width() - FBT_W;
    y0 = fb_height() - FBT_H;
    for (uint32_t y = 0; y < FBT_H; y++)
        for (uint32_t x = 0; x < FBT_W; x++) fbt_saved[y][x] = fb_get_pixel(x0 + x, y0 + y);
    saved = 1;

    const uint32_t fg = 0x00C08040u, bg = 0x00204060u;
    const uint32_t pfg = fb_rgb(0xC0, 0x80, 0x40), pbg = fb_rgb(0x20, 0x40, 0x60);

    /* text: three cells of fg ink on bg, the glyph's ink the smaller part (so
       fg and bg are not swapped), nothing past the third cell, and the count
       of characters back. A kernel pointer is refused. */
    uint32_t cw = fb_font_width(), ch = fb_font_height();
    step++;
    if (3 * cw >= FBT_W || ch > FBT_H) goto done;
    fb_fill_rect(x0, y0, FBT_W, FBT_H, 0);
    step++;
    if (syscall_dispatch(SYS_FB_DRAW_TEXT, x0, y0, FBT_TEXT, fg, bg) != 3) goto done;
    uint32_t nfg = 0, nbg = 0;
    for (uint32_t y = 0; y < ch; y++) {
        for (uint32_t x = 0; x < 3 * cw; x++) {
            uint32_t p = fb_get_pixel(x0 + x, y0 + y);
            if (p == pfg) nfg++;
            else if (p == pbg) nbg++;
        }
    }
    step++;
    if (nfg + nbg != 3 * cw * ch || nfg == 0 || nfg >= nbg) goto done;
    step++;
    if (fb_get_pixel(x0 + 3 * cw, y0) != 0) goto done;
    step++;
    if (syscall_dispatch(SYS_FB_DRAW_TEXT, x0, y0, 0xC0001000u, fg, bg) != E_BAD_ADDRESS) goto done;

    fb_fill_rect(x0, y0, FBT_W, FBT_H, 0);

    /* a pixel at (3, 1), and none at (1, 3) */
    step++;
    if (syscall_dispatch(SYS_FB_PUT_PIXEL, x0 + 3, y0 + 1, fg, 0, 0) != E_OK) goto done;
    step++;
    if (fb_get_pixel(x0 + 3, y0 + 1) != pfg || fb_get_pixel(x0 + 1, y0 + 3) != 0) goto done;

    /* a rectangle 4 wide and 2 high at (8, 2): its far corner in, one past
       each edge out */
    step++;
    if (syscall_dispatch(SYS_FB_FILL_RECT, x0 + 8, y0 + 2, 4, 2, fg) != E_OK) goto done;
    step++;
    if (fb_get_pixel(x0 + 8, y0 + 2) != pfg || fb_get_pixel(x0 + 11, y0 + 3) != pfg) goto done;
    step++;
    if (fb_get_pixel(x0 + 12, y0 + 2) != 0 || fb_get_pixel(x0 + 8, y0 + 4) != 0) goto done;

    /* a line from (16, 1) to (20, 1): both ends drawn, not the column down */
    step++;
    if (syscall_dispatch(SYS_FB_DRAW_LINE, x0 + 16, y0 + 1, x0 + 20, y0 + 1, fg) != E_OK) goto done;
    step++;
    if (fb_get_pixel(x0 + 16, y0 + 1) != pfg || fb_get_pixel(x0 + 20, y0 + 1) != pfg) goto done;
    step++;
    if (fb_get_pixel(x0 + 16, y0 + 5) != 0) goto done;

    /* the font name reaches the name rules (which refuse this one) */
    step++;
    if (syscall_dispatch(SYS_FB_SET_FONT, FBT_NAME, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "framebuffer syscall test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (saved)
        for (uint32_t y = 0; y < FBT_H; y++)
            for (uint32_t x = 0; x < FBT_W; x++) fb_put_pixel(x0 + x, y0 + y, fbt_saved[y][x]);
    thread_set_caps(me, save);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- advisory file locks ---------------------------------------------------
 * FILE_OP_LOCK's contract, the parts a regression would break quietly:
 * - Shared locks coexist and exclusive ones do not. The lock belongs to the
 *   open file, so a second open in the same process conflicts like anyone's.
 * - Closing a handle releases its locks without an UNLOCK.
 * - Byte ranges: disjoint ranges coexist, len 0 runs to the end, and an
 *   unlock does not split a lock.
 * - FLOCK_WAIT sleeps until a release wakes it, and refuses with E_DEADLOCK
 *   where waiting could never end: on its own lock, and round a cycle with
 *   another thread.
 * - Advisory: a locked file can still be written and unlinked, and the old
 *   on-disk lock bytes, set to a live owner as a crafted volume could, are
 *   ignored.
 * - Unlinked while open: the name goes at once, the entry stays (and its id
 *   is not handed to the next file created) until the last close.
 *
 * The helper thread makes its calls from its own part of the test page, so a
 * preemption between the two threads cannot mix up their arguments.
 */
#define FL_ARGS  (TEST_CODE_VIRT + 0x800)
#define FL_PATH  (TEST_CODE_VIRT + 0x900)

static volatile int fl_stage;      /* how far the helper has got */
static volatile int fl_go;         /* set once its caps are in place */
static volatile int fl_rc;         /* what its waiting lock returned */

static int fl_call(uint32_t op, int handle, int32_t off, uint32_t len, uint32_t flags) {
    struct file_op_args *a = (struct file_op_args *)FL_ARGS;
    a->op = op; a->handle = handle; a->path = FL_PATH; a->path2 = 0;
    a->data = 0; a->len = len; a->off = off; a->flags = flags;
    return syscall_dispatch(SYS_FILE_OP, FL_ARGS, 0, 0, 0, 0);
}

/* Holds [10,20), then waits for [0,10), which the test thread holds. */
static void fl_helper(void) {
    while (!fl_go) yield();               /* may run before thread_set_caps */
    int h = fl_call(FILE_OP_OPEN, 0, 0, 0, FOPEN_READ);
    if (h < 0) { fl_rc = h; fl_stage = -1; return; }
    if (fl_call(FILE_OP_LOCK, h, 10, 10, FLOCK_EXCLUSIVE) != E_OK) { fl_stage = -2; return; }
    fl_stage = 1;
    fl_rc = fl_call(FILE_OP_LOCK, h, 0, 10, FLOCK_EXCLUSIVE | FLOCK_WAIT);
    fl_stage = 2;
    fl_call(FILE_OP_CLOSE, h, 0, 0, 0);
    fl_stage = 3;
}

int usermode_flock_test(void) {
    if (!cxfs_is_mounted()) return 1;         /* nothing mounted - skip */

    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;

    int      me   = thread_current_id();
    uint32_t save = thread_current_caps();
    thread_set_caps(me, GRANT_DISK);

    int ok = 0, step = 0;
    int a = -1, b = -1, w = -1, id, other, closed, helper;
    struct cxfs_entry e;
    const char *scratch = (cxfs_resolve("/Temp", 0) >= 0) ? "/Temp" : "";
    fl_stage = 0;
    fl_go = 0;
    ft_strp(FT_PATH, scratch, "/kt_lock.txt");

    a = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
    b = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;                                                                       /* 1 */
    if (a < 0 || b < 0) goto done;

    /* --- whole file: shared with shared, exclusive with nothing --- */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, FLOCK_SHARED) != E_OK) goto done;
    step++;
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 0, 0, FLOCK_SHARED) != E_OK) goto done;
    step++;                                                                       /* 4 */
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_BUSY) goto done;
    /* a refused conversion keeps the lock it had: a still conflicts with b */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_BUSY) goto done;
    step++;
    if (ft_call(FILE_OP_UNLOCK, a, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 8 */
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, FLOCK_SHARED) != E_BUSY) goto done;
    /* waiting on a lock this same process holds could never end */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, FLOCK_SHARED | FLOCK_WAIT) != E_DEADLOCK) goto done;

    /* advisory: a write through another handle is not refused */
    ft_str(FT_DATA, "locked..............");
    step++;                                                                       /* 10 */
    if (ft_call(FILE_OP_WRITE, a, 0, FT_DATA, 20, 0, 0) != 20) goto done;

    /* closing b, with no UNLOCK, releases its lock */
    step++;
    if (ft_call(FILE_OP_CLOSE, b, 0, 0, 0, 0, 0) != E_OK) goto done;
    closed = b;
    b = -1;
    step++;                                                                       /* 12 */
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;

    /* bad arguments */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, 3) != E_INVALID_ARGS) goto done;
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, -1, FLOCK_SHARED) != E_INVALID_ARGS) goto done;
    step++;                                                                       /* 15 */
    if (ft_call(FILE_OP_LOCK, closed, 0, 0, 0, 0, FLOCK_SHARED) != E_BAD_HANDLE) goto done;
    ft_call(FILE_OP_UNLOCK, a, 0, 0, 0, 0, 0);

    /* --- byte ranges --- */
    b = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;
    if (b < 0) goto done;
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 10, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 18 */
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 10, 10, FLOCK_EXCLUSIVE) != E_OK) goto done;   /* disjoint */
    step++;
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 2, 9, FLOCK_SHARED) != E_BUSY) goto done;     /* overlaps a */
    /* an unlock inside a lock does not split it */
    step++;                                                                       /* 20 */
    if (ft_call(FILE_OP_UNLOCK, a, 0, 0, 5, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 1, 0, FLOCK_SHARED) != E_BUSY) goto done;
    step++;
    if (ft_call(FILE_OP_UNLOCK, a, 0, 0, 10, 0, 0) != E_OK) goto done;
    step++;                                                                       /* 23 */
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 1, 0, FLOCK_SHARED) != E_OK) goto done;
    /* len 0 runs to the end of the file, past anything written */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 100, FLOCK_SHARED) != E_OK) goto done;
    step++;                                                                       /* 25 */
    if (ft_call(FILE_OP_LOCK, b, 0, 0, 10, 5000, FLOCK_EXCLUSIVE) != E_BUSY) goto done;
    ft_call(FILE_OP_UNLOCK, a, 0, 0, 0, 0, 0);
    ft_call(FILE_OP_CLOSE, b, 0, 0, 0, 0, 0);
    b = -1;

    /* --- FLOCK_WAIT sleeps, a deadlock cycle is refused, a release wakes --- */
    step++;                                                                       /* 26 */
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 10, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    ft_strp(FL_PATH, scratch, "/kt_lock.txt");
    fl_rc = 1;
    helper = thread_create("ktlock", fl_helper);
    step++;
    if (helper < 0) goto done;
    thread_set_caps(helper, GRANT_DISK);
    fl_go = 1;
    for (int i = 0; i < 50 && fl_stage == 0; i++) yield();
    for (int i = 0; i < 10; i++) yield();     /* let it reach the wait */
    step++;                                                                       /* 28 */
    if (fl_stage != 1) goto done;             /* it must be asleep, not through */
    /* the helper holds [10,20) and waits on our [0,10): our wait on [10,20)
       would close the cycle */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 10, 10, FLOCK_EXCLUSIVE | FLOCK_WAIT) != E_DEADLOCK) goto done;
    step++;                                                                       /* 30 */
    if (ft_call(FILE_OP_UNLOCK, a, 0, 0, 10, 0, 0) != E_OK) goto done;
    for (int i = 0; i < 50 && fl_stage < 3; i++) yield();
    step++;
    if (fl_stage != 3 || fl_rc != E_OK) goto done;

    /* --- the old on-disk lock, owned by a live thread (us): ignored --- */
    id = cxfs_resolve(ft_join(scratch, "/kt_lock.txt"), 0);
    step++;                                                                       /* 32 */
    if (id < 0 || cxfs_read_entry((uint32_t)id, &e) != 0) goto done;
    e.reserved_9e[0] = 1;                                   /* lock_state */
    *(uint32_t *)&e.reserved_9e[2] = (uint32_t)me;          /* lock_owner_pid */
    if (cxfs_write_entry(&e) != 0) goto done;
    w = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ | FOPEN_WRITE);
    step++;
    if (w < 0) goto done;
    step++;                                                                       /* 34 */
    if (ft_call(FILE_OP_WRITE, w, 0, FT_DATA, 6, 0, 0) != 6) goto done;

    /* --- unlinked while open: locked, still unlinkable, entry kept --- */
    step++;
    if (ft_call(FILE_OP_LOCK, a, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 36 */
    if (ft_call(FILE_OP_UNLINK, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(FILE_OP_STAT, 0, FT_PATH, FT_STAT, 0, 0, 0) != E_NOT_FOUND) goto done;
    step++;                                                                       /* 38 */
    if (ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ) != E_NOT_FOUND) goto done;
    step++;
    if (ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_CREATE) != E_ALREADY_EXISTS) goto done;
    /* the open handles still work */
    step++;                                                                       /* 40 */
    if (ft_call(FILE_OP_SEEK, w, 0, 0, 0, 0, FSEEK_SET) != 0) goto done;
    step++;
    if (ft_call(FILE_OP_READ, w, 0, FT_DATA, 6, 0, 0) != 6) goto done;
    /* exec_path resolves for itself, and must not find it either (it would
       otherwise reach the signature check: E_INVALID_ARGS, not E_NOT_FOUND) */
    {
        struct spawn_args *sa = (struct spawn_args *)FT_STAT;
        sa->image = 0; sa->image_len = 0; sa->name = FT_PATH;
        sa->broker_endpoint = -1; sa->caps = 0; sa->args = 0; sa->args_len = 0;
        step++;                                                                   /* 42 */
        if (sys_exec_path(FT_PATH, user_addr(sa)) != E_NOT_FOUND) goto done;
    }
    /* and the id is not handed to the next file created */
    ft_strp(FT_PATH, scratch, "/kt_lock2.txt");
    other = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_CREATE);
    step++;                                                                       /* 43 */
    if (other < 0) goto done;
    ft_call(FILE_OP_CLOSE, other, 0, 0, 0, 0, 0);
    step++;
    if (cxfs_resolve(ft_join(scratch, "/kt_lock2.txt"), 0) == id) goto done;
    /* the entry goes with the last close, not the first */
    ft_call(FILE_OP_CLOSE, w, 0, 0, 0, 0, 0);
    w = -1;
    step++;                                                                       /* 45 */
    if (cxfs_read_entry((uint32_t)id, &e) != 0 || e.type != CXFS_TYPE_FILE) goto done;
    ft_call(FILE_OP_CLOSE, a, 0, 0, 0, 0, 0);
    a = -1;
    step++;
    if (cxfs_read_entry((uint32_t)id, &e) == 0 && e.type != CXFS_TYPE_FREE) goto done;
    /* and the name is free again */
    ft_strp(FT_PATH, scratch, "/kt_lock.txt");
    w = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_CREATE);
    step++;                                                                       /* 47 */
    if (w < 0) goto done;

    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "file lock test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    htab_close_all(thread_htab(me));
    /* a helper still waiting is woken by the release above; let it finish */
    for (int i = 0; i < 50 && fl_stage > 0 && fl_stage < 3; i++) yield();
    ft_call(FILE_OP_SYNC, 0, 0, 0, 0, 0, 0);                /* runs any deferred delete */
    if ((id = cxfs_resolve(ft_join(scratch, "/kt_lock.txt"), 0))  >= 0) cxfs_delete_entry((uint32_t)id);
    if ((id = cxfs_resolve(ft_join(scratch, "/kt_lock2.txt"), 0)) >= 0) cxfs_delete_entry((uint32_t)id);
    thread_set_caps(me, save);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- Checkpoint 3b: scheduler-integrated ring-3 processes ---- */
struct ring3_setup {
    const void *blob;
    uint32_t    blob_len;
    const char *msg;
    uint32_t    code_virt;
    uint32_t    stack_virt;
    paddr_t     code_phys;     /* pmm frame backing the code page (for free) */
    paddr_t     stack_phys;    /* pmm frame backing the stack page (for free) */
};

/* The thread's own record (thread_set_arg), freed when it is reaped. */
static void process_trampoline(void) {
    int pid = thread_current_id();
    struct ring3_setup *s = thread_current_arg();

    /* back this process's pages with pmm frames, mapped user-accessible */
    s->code_phys  = map_user_page(s->code_virt);
    s->stack_phys = map_user_page(s->stack_virt);
    if (!s->code_phys || !s->stack_phys) {
        if (s->code_phys)  unmap_user_page(s->code_virt, s->code_phys);
        if (s->stack_phys) unmap_user_page(s->stack_virt, s->stack_phys);
        klog_u32("USERMODE", SEV_ERR, "PROCESS ", (uint32_t)pid, LOG_COLOR_VALUE, " OUT OF MEM");
        thread_exit();
    }

    uint32_t len = s->blob_len;
    if (len == 0 || len > USER_MSG_OFF) len = USER_MSG_OFF;
    uint8_t *dst = (uint8_t *)user_ptr(s->code_virt);
    const uint8_t *src = (const uint8_t *)s->blob;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    char *umsg = (char *)user_ptr(s->code_virt + USER_MSG_OFF);
    uint32_t i = 0;
    if (s->msg) { for (; s->msg[i] && i < 0x100; i++) umsg[i] = s->msg[i]; }
    umsg[i] = '\0';
    int sealed = seal_user_code(s->code_virt, s->code_phys) == 0;

    uint32_t ustack_top = s->stack_virt + 0x1000 - 16;
    *(uint32_t *)user_ptr(ustack_top) = user_addr(umsg);

    if (sealed) enter_usermode(s->code_virt, ustack_top, thread_current_usave());

    /* user routine SYS_EXITed or faulted: save a fault noted on the way out
       (out of the exception now), reclaim its pages, leave the scheduler. */
    crashlog_save_faults();
    unmap_user_page(s->code_virt, s->code_phys);
    unmap_user_page(s->stack_virt, s->stack_phys);
    s->code_virt = 0;
    thread_exit();   /* never returns */
}

/* Everything but starting it, so a caller can finish configuring the process
   (its uid, say) before it can run. See thread_create_process. */
static int process_prepare_ring3(const char *name,
                                 const void *blob, uint32_t blob_len,
                                 const char *msg) {
    int pid = thread_create_process(name, process_trampoline);
    if (pid < 0) return -1;
    struct ring3_setup *s = (pid < R3_SLOTS) ? kmalloc(sizeof *s) : 0;
    if (!s) { thread_discard(pid); return -1; }
    s->blob       = blob;
    s->blob_len   = blob_len;
    s->msg        = msg;
    s->code_virt  = R3_BASE + (uint32_t)pid * R3_SLOT_SIZE;
    s->stack_virt = s->code_virt + 0x1000;
    s->code_phys  = 0;
    s->stack_phys = 0;
    thread_set_arg(pid, s, kfree);
    thread_mark_user(pid);
    thread_set_caps(pid, GRANT_CONSOLE);   /* SYSTEM ring-3 helper: may write console */
    klog_u32("RING3", SEV_INFO, "SYSMODE CALL - pid ", (uint32_t)pid, LOG_COLOR_VALUE, "");
    return pid;
}

int process_create_ring3(const char *name,
                         const void *blob, uint32_t blob_len,
                         const char *msg) {
    int pid = process_prepare_ring3(name, blob, blob_len, msg);
    if (pid < 0) return -1;
    /* runs as SYSTEM: this is the kernel launching a ring-3 helper as the
       machine identity. To launch on behalf of a human user, use
       process_create_ring3_as_user(). */
    thread_start(pid);
    return pid;
}

/* Launch a ring-3 process owned by a specific user. Enforces the core identity
   invariant: a user is ALWAYS UID >= 1 - launching a user process as UID 0
   (SYSTEM) is rejected. Returns the pid, or -1 (incl. if uid == SYSTEM). */
int process_create_ring3_as_user(const char *name, const void *blob, uint32_t blob_len, const char *msg, uint32_t uid) {
    if (!uid_is_user(uid)) return -1;   /* user can never be UID 0 / invalid */
    int pid = process_prepare_ring3(name, blob, blob_len, msg);
    if (pid < 0) return -1;
    /* pid is already marked is_user by process_prepare_ring3, so thread_set_uid's
       guard would block UID 0 here too - but uid is validated >= 1 above. Set
       while the thread is still held: started first, it could run as SYSTEM
       before this line did. */
    thread_set_uid(pid, uid);
    thread_start(pid);
    klog_u32("RING3", SEV_INFO, "USERMODE CALL - uid ", uid, LOG_COLOR_VALUE, "");
    return pid;
}

/* ---- user fault handler (registered with the IDT) ----
 * A CPU exception while in ring 3 kills the offending process instead of
 * panicking the kernel - a buggy user program can't take the system down.
 *
 * It leaves the way SYS_EXIT does: back out of enter_usermode, into whatever
 * entered ring 3, which then cleans up as after any exit. It used to call
 * thread_exit() from here, which skipped that cleanup: a spawned process's
 * whole address space, or a ring-3 helper's two pages, leaked on every fault.
 * Safe because the fault came from ring 3: this kernel stack holds only the
 * trap frame, no lock and nothing half done, and the saved EFLAGS come back. */
static atomic_u32 user_faults;

uint32_t usermode_faults(void) { return atomic_load(&user_faults); }

static void usermode_fault(struct registers *r) {
    int pid = thread_current_pid();

    atomic_fetch_add(&user_faults, 1);
    klog_u32("USERMODE", SEV_ERR, "user process ", (uint32_t)pid, LOG_COLOR_VALUE, " faulted");
    uint32_t vector = irq_frame_vector(r);
    klog_child_u32("exception: ", vector, LOG_COLOR_VALUE, "");

    /* Where, and why: without these a fault on real hardware, which has no
       debugger, says only that something went wrong. CR2 matters only for a
       page fault (14), and reading it later would see some other fault's. */
    char line[64];
    size_t n = 0;
    const char *ip = "at 0x", *err = "  error 0x", *addr = "  address 0x";
    while (*ip) line[n++] = *ip++;
    n += fmt_hex(line + n, (uint32_t)irq_frame_ip(r), 8, 0);
    while (*err) line[n++] = *err++;
    n += fmt_hex(line + n, irq_frame_error(r), 0, 0);
    if (vector == 14) {
        while (*addr) line[n++] = *addr++;
        n += fmt_hex(line + n, (uint32_t)irq_fault_address(), 8, 0);
    }
    line[n] = 0;
    klog_child(line);
    klog_child("process terminated");

    /* The same, as one line for the crash area: "pid 2 (name) exception 13
       at 0x... error 0x...". Saved by this thread once it is out of ring 3
       (crashlog_save_faults), not from here. */
    char rec[128];
    size_t m = 0;
    const char *name = thread_name(thread_current_id());
    #define REC_PUT(s) do { const char *p_ = (s); while (*p_ && m < sizeof rec - 1) rec[m++] = *p_++; } while (0)
    char num[12];
    REC_PUT("pid ");
    fmt_u32(num, (uint32_t)pid); REC_PUT(num);
    if (name) { REC_PUT(" ("); REC_PUT(name); REC_PUT(")"); }
    REC_PUT(" exception ");
    fmt_u32(num, vector); REC_PUT(num);
    REC_PUT(" ");
    REC_PUT(line);
    #undef REC_PUT
    rec[m] = 0;
    crashlog_note_fault(rec);

    return_to_kernel(USERMODE_FAULTED, thread_current_usave());   /* never returns */
}

void usermode_register_fault_handler(void) {
    set_user_fault_hook(usermode_fault);
}