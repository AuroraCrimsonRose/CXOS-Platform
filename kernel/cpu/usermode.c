// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/usermode.c */
/* Aurora Tejeda / CATX Systems */
/* Ring 3 entry, syscall dispatch, and the user-mode tests (Checkpoints 3a/3b). */

#include "usermode.h"
#include "arch/irq.h"        /* set_user_fault_hook, irq_frame_* */
#include "arch/cpu_ops.h"    /* arch_irq_save, around the system page's count */
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
#include "job.h"
#include "color.h"
#include "fb.h"
#include "caps.h"
#include "object/htab.h"
#include "object/kwait.h"     /* kobj_wait, for SYS_OBJECT_WAIT */
#include "object/ktimer.h"    /* SYS_TIMER_* */
#include "sync/atomic.h"
#include "sysrec.h"
#include "systable.h"         /* the handlers, decoders and table (generated) */
#include "syspage.h"          /* the kernel space's system page, the origin check */
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

int syscall_console_write(uint64_t buf, uint64_t len64) {
    if (!(thread_current_caps() & GRANT_CONSOLE)) {
        klog_u32("GRANT", SEV_WARN, "console_write DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_CONSOLE)");
        return E_ACCESS_DENIED;
    }
    /* No buffer reaches 4 GB on x86-32, and none worth printing does on x86-64:
       refuse a length past 32 bits rather than print its low half. */
    if (len64 > 0xFFFFFFFFu) return E_OUT_OF_RANGE;
    uint32_t len = (uint32_t)len64;
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

int syscall_log_read(uint32_t i, uint64_t e) {
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
    proc_note_exit(thread_proc(thread_current_id()), code);
    return_to_kernel((int)code, thread_current_usave());
}

void syscall_yield(void) { yield(); }

int syscall_clock(uint64_t c) {
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

int syscall_args(uint64_t a) {
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

int syscall_clock_monotonic(uint64_t now) {
    /* Unprivileged, as SYS_CLOCK is: the time since boot is not authority. */
    uint64_t t = timer_now_ns();
    if (user_copy_out(now, &t, sizeof t) != 0) return E_BAD_ADDRESS;
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

int syscall_spawn(uint64_t a) {
    if (!(thread_current_caps() & GRANT_SPAWN)) return E_ACCESS_DENIED;
    return sys_spawn(a);
}

int syscall_exec_path(uint64_t path, uint64_t a) {
    if (!(thread_current_caps() & GRANT_SPAWN)) return E_ACCESS_DENIED;
    return sys_exec_path(path, a);
}

/* The eighteen file calls share one gate, as the network calls do. */
static int disk_granted(void) {
    if (thread_current_caps() & GRANT_DISK) return 1;
    klog_u32("GRANT", SEV_WARN, "file DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_DISK)");
    return 0;
}

int syscall_file_open(uint64_t path, uint32_t flags) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_open(path, flags);
}

int syscall_file_close(int32_t h) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_close(h);
}

int syscall_file_read(int32_t h, uint64_t buf, uint64_t n) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_read(h, buf, n);
}

int syscall_file_write(int32_t h, uint64_t buf, uint64_t n) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_write(h, buf, n);
}

int syscall_file_seek(int32_t h, int64_t off, uint32_t origin, uint64_t pos) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_seek(h, off, origin, pos);
}

int syscall_file_tell(int32_t h, uint64_t pos) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_tell(h, pos);
}

int syscall_file_truncate(int32_t h, uint64_t size) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_truncate(h, size);
}

int syscall_file_stat(uint64_t path, uint64_t st) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_stat(path, st);
}

int syscall_file_fstat(int32_t h, uint64_t st) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_fstat(h, st);
}

int syscall_file_readdir(uint64_t path, uint32_t index, uint64_t st) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_readdir(path, index, st);
}

int syscall_file_mkdir(uint64_t path) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_mkdir(path);
}

int syscall_file_unlink(uint64_t path) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_unlink(path);
}

int syscall_file_rename(uint64_t path, uint64_t newname) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_rename(path, newname);
}

int syscall_file_chdir(uint64_t path) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_chdir(path);
}

int syscall_file_getcwd(uint64_t buf, uint64_t cap) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_getcwd(buf, cap);
}

int syscall_file_sync(void) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_sync();
}

int syscall_file_lock(int32_t h, uint64_t a) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_lock(h, a);
}

int syscall_file_unlock(int32_t h, uint64_t start, uint64_t len) {
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_unlock(h, start, len);
}

/* The seven framebuffer calls share one gate, as the power calls do. */
static int fb_granted(void) {
    if (thread_current_caps() & GRANT_FRAMEBUFFER) return 1;
    klog_u32("GRANT", SEV_WARN, "framebuffer DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_FRAMEBUFFER)");
    return 0;
}

int syscall_fb_info(uint64_t g) {
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

int syscall_fb_draw_text(uint32_t x, uint32_t y, uint64_t text, uint32_t fg, uint32_t bg) {
    if (!fb_granted()) return E_ACCESS_DENIED;
    return sys_fb_draw_text(x, y, text, fg, bg);
}

int syscall_fb_set_font(uint64_t name) {
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

/* The ten network calls share one gate, as the framebuffer calls do. */
static int net_granted(void) {
    if (thread_current_caps() & GRANT_NET) return 1;
    klog_u32("GRANT", SEV_WARN, "net DENIED for pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, " (no GRANT_NET)");
    return 0;
}

int syscall_net_status(void) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_status();
}

int syscall_net_mac(uint64_t buf, uint64_t len) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_mac(buf, len);
}

int syscall_net_get_config(uint64_t c) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_get_config(c);
}

int syscall_net_set_ip(uint32_t addr) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_set_ip(addr);
}

int syscall_net_set_mask(uint32_t addr) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_set_mask(addr);
}

int syscall_net_set_gateway(uint32_t addr) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_set_gateway(addr);
}

int syscall_net_set_dns(uint32_t addr) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_set_dns(addr);
}

int syscall_net_ping(uint32_t dst, uint32_t seq, uint64_t rtt) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_ping(dst, seq, rtt);
}

int syscall_net_send(uint64_t frame, uint64_t len) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_send(frame, len);
}

int syscall_net_recv(uint64_t buf, uint64_t cap) {
    if (!net_granted()) return E_ACCESS_DENIED;
    return sys_net_recv(buf, cap);
}

int syscall_mouse_read(uint64_t m) {
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
int syscall_vm_map(uint64_t a) {
    if (!user_ptr_writable(a, sizeof(struct vm_map_args))) return E_BAD_ADDRESS;
    return vm_map(thread_current_pid(), (struct vm_map_args *)user_ptr(a));
}

int syscall_vm_unmap(uint64_t addr, uint64_t length) {
    return vm_unmap(thread_current_pid(), addr, length);
}

int syscall_vm_info(uint64_t u) {
    if (!user_ptr_writable(u, sizeof(struct vm_usage))) return E_BAD_ADDRESS;
    return vm_info(thread_current_pid(), (struct vm_usage *)user_ptr(u));
}

int syscall_ipc_call(uint64_t a)  { return ipc_call(a); }
int syscall_ipc_recv(uint64_t a)  { return ipc_recv(a); }
int syscall_ipc_reply(uint64_t a) { return ipc_reply(a); }

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

int syscall_object_info(uint64_t a) {
    /* Unprivileged: it describes only a handle the caller holds. */
    struct object_info_args oa;
    if (user_copy_in(&oa, a, sizeof oa) != 0) return E_BAD_ADDRESS;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), oa.handle,
                              KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    if (oa.topic == OBJINFO_PROCESS) {
        /* Read while the handle's reference holds the record. */
        struct proc *p = (struct proc *)o;
        int rc = o->type != KOBJ_PROCESS          ? E_WRONG_TYPE
               : !(hi.rights & HRIGHT_INSPECT)    ? E_ACCESS_DENIED
               : oa.cap < sizeof(struct process_info) ? E_BUFFER_TOO_SMALL : E_OK;
        struct process_info pi = { 0 };
        if (rc == E_OK) {
            /* How it ended only once it has: a note waits for the last thread. */
            int done = (atomic_load(&o->signals) & SIGNAL_PROCESS_ENDED) != 0;
            pi.pid    = (uint32_t)p->pid;
            pi.ended  = done ? p->ended : PROC_RUNNING;
            pi.code   = done ? p->end_code : 0;
            pi.vector = done ? p->end_vector : 0;
            pi.ip     = done ? p->end_ip : 0;
            pi.addr   = done ? p->end_addr : 0;
            pi.killer = done ? p->end_killer : 0;
        }
        kobj_unref(o);
        if (rc != E_OK) return rc;
        if (user_copy_out(oa.buf, &pi, sizeof pi) != 0) return E_BAD_ADDRESS;
        return (int)sizeof pi;
    }
    if (oa.topic == OBJINFO_JOB) {
        struct job *j = (struct job *)o;
        int rc = o->type != KOBJ_JOB              ? E_WRONG_TYPE
               : !(hi.rights & HRIGHT_INSPECT)    ? E_ACCESS_DENIED
               : oa.cap < sizeof(struct job_info) ? E_BUFFER_TOO_SMALL : E_OK;
        struct job_info ji = { 0 };
        if (rc == E_OK) {
            /* One moment's account: the counts change with interrupts off. */
            uint32_t f = arch_irq_save();
            ji.memory_limit = j->mem_limit;
            ji.memory_used  = j->mem_used;
            ji.memory_peak  = j->mem_peak;
            ji.thread_limit = j->thread_limit;
            ji.threads      = j->threads;
            ji.processes    = j->procs;
            ji.killed       = (uint32_t)j->killed;
            arch_irq_restore(f);
        }
        kobj_unref(o);
        if (rc != E_OK) return rc;
        if (user_copy_out(oa.buf, &ji, sizeof ji) != 0) return E_BAD_ADDRESS;
        return (int)sizeof ji;
    }
    struct object_basic b = {
        .kind = o->type, .rights = hi.rights, .koid = o->koid,
        .grant = hi.grant, .parent_grant = hi.parent,
    };
    kobj_unref(o);
    if (oa.topic != OBJINFO_BASIC) return E_INVALID_ARGS;
    if (oa.cap < sizeof b) return E_BUFFER_TOO_SMALL;
    if (user_copy_out(oa.buf, &b, sizeof b) != 0) return E_BAD_ADDRESS;
    return (int)sizeof b;
}

int syscall_object_wait(int32_t h, uint32_t signals, uint64_t deadline, uint64_t seen) {
    /* Unprivileged: the handle's HRIGHT_WAIT is the authority. Any deadline
       is accepted: a far one costs nothing a wait with none does not
       (kobj_wait blocks in pieces), so one past DEADLINE_NEVER is as good. */
    if (seen && !user_ptr_writable(seen, sizeof(uint32_t))) return E_BAD_ADDRESS;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    const struct kobj_ops *ops = kobj_type_ops((enum kobj_type)o->type);
    int rc = !(hi.rights & HRIGHT_WAIT)                 ? E_ACCESS_DENIED
           : !ops->signals_valid                         ? E_NOT_SUPPORTED
           : !signals || (signals & ~ops->signals_valid) ? E_INVALID_ARGS : E_OK;
    if (rc != E_OK) { kobj_unref(o); return rc; }
    uint32_t s = 0;
    rc = kobj_wait(o, signals, deadline, &s);
    kobj_unref(o);
    /* Written after the wait, through the copy that checks the range again:
       the page may have been unmapped while this thread slept. */
    if (seen && user_copy_out(seen, &s, sizeof s) != 0) return E_BAD_ADDRESS;
    return rc;
}

int syscall_timer_create(void) {
    /* Unprivileged: a timer is a handle, held to the handle table's limit. */
    struct ktimer *t = ktimer_create();
    if (!t) return E_NO_MEMORY;
    int32_t h = htab_install(thread_htab(thread_current_id()), &t->hdr,
                             HRIGHT_WAIT | HRIGHT_ARM | HRIGHT_DUPLICATE, 0);
    kobj_unref(&t->hdr);   /* the handle's reference keeps it; with no handle, it goes */
    return h;
}

/* The timer behind `h`, with a reference, if the handle may arm it. */
static struct ktimer *timer_from_handle(int32_t h, int *rc) {
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) { *rc = E_BAD_HANDLE; return 0; }
    *rc = o->type != KOBJ_TIMER     ? E_WRONG_TYPE
        : !(hi.rights & HRIGHT_ARM) ? E_ACCESS_DENIED : E_OK;
    if (*rc != E_OK) { kobj_unref(o); return 0; }
    return (struct ktimer *)o;
}

int syscall_timer_set(int32_t h, uint64_t deadline) {
    /* Unprivileged: the handle's HRIGHT_ARM is the authority. */
    if (deadline >= DEADLINE_NEVER) return E_INVALID_ARGS;   /* cancel it instead */
    int rc;
    struct ktimer *t = timer_from_handle(h, &rc);
    if (!t) return rc;
    ktimer_set(t, deadline);
    kobj_unref(&t->hdr);
    return E_OK;
}

int syscall_timer_cancel(int32_t h) {
    int rc;
    struct ktimer *t = timer_from_handle(h, &rc);
    if (!t) return rc;
    ktimer_cancel(t);
    kobj_unref(&t->hdr);
    return E_OK;
}

int syscall_process_kill(int32_t h) {
    /* Unprivileged: the handle's HRIGHT_KILL is the authority. */
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_PROCESS     ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_KILL)  ? E_ACCESS_DENIED : E_OK;
    if (rc == E_OK) {
        struct proc *p = (struct proc *)o;
        if (p->running) {
            klog_u32("USERMODE", SEV_INFO, "pid ", (uint32_t)p->pid, LOG_COLOR_VALUE, " killed");
            klog_child_u32("by pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, "");
        }
        proc_kill(p, thread_current_pid());
    }
    kobj_unref(o);
    return rc;
}

int syscall_process_stop(int32_t h, uint32_t deadline_ms) {
    /* Unprivileged: the handle's HRIGHT_STOP is the authority. */
    if (deadline_ms > SLEEP_MAX_MS) return E_OUT_OF_RANGE;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_PROCESS     ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_STOP)  ? E_ACCESS_DENIED : E_OK;
    if (rc == E_OK) {
        struct proc *p = (struct proc *)o;
        if (p->running) {
            klog_u32("USERMODE", SEV_INFO, "pid ", (uint32_t)p->pid, LOG_COLOR_VALUE, " asked to stop");
            klog_child_u32("by pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, "");
        }
        proc_stop(p, thread_current_pid(), deadline_ms);
    }
    kobj_unref(o);
    return rc;
}

int syscall_process_stop_endpoint(int32_t h) {
    /* Unprivileged: a process names one of its own receive endpoints. */
    struct proc *me = thread_proc(thread_current_id());
    if (!me) return E_INTERNAL;
    if (h == 0) { proc_set_stop_endpoint(me, 0); return E_OK; }
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_ENDPOINT    ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_RECV)  ? E_ACCESS_DENIED : E_OK;
    if (rc == E_OK) proc_set_stop_endpoint(me, (struct endpoint *)o);
    kobj_unref(o);
    return rc;
}

/* *limits from the caller, or none of the job's own for 0. */
static int job_limits_in(uint64_t u, struct job_limits *l) {
    l->memory = 0; l->threads = 0; l->reserved = 0;
    if (!u) return E_OK;
    if (user_copy_in(l, u, sizeof *l) != 0) return E_BAD_ADDRESS;
    return l->reserved ? E_INVALID_ARGS : E_OK;
}

/* The job `h` names, with a reference, if it has `right`; else NULL and *rc. */
static struct job *job_from_handle(int32_t h, uint32_t right, int *rc) {
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    *rc = !o                       ? E_BAD_HANDLE
        : o->type != KOBJ_JOB      ? E_WRONG_TYPE
        : !(hi.rights & right)     ? E_ACCESS_DENIED : E_OK;
    if (*rc == E_OK) return (struct job *)o;
    if (o) kobj_unref(o);
    return 0;
}

int syscall_job_create(int32_t parent, uint64_t limits) {
    /* Unprivileged: a job only divides what its parent has. */
    struct job_limits l;
    int rc = job_limits_in(limits, &l);
    if (rc != E_OK) return rc;
    struct job *top;
    if (parent == 0) {
        struct proc *me = thread_proc(thread_current_id());
        if (!me) return E_INTERNAL;
        top = me->job;
        kobj_ref(&top->hdr);
    } else if (!(top = job_from_handle(parent, HRIGHT_ASSIGN, &rc))) {
        return rc;
    }
    struct job *j = job_create(top, l.memory, l.threads, &rc);
    job_put(top);   /* the new job holds its own on its parent */
    if (!j) return rc;
    int32_t h = htab_install(thread_htab(thread_current_id()), &j->hdr,
                             HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_KILL | HRIGHT_ASSIGN
                             | HRIGHT_SET_LIMITS | HRIGHT_DUPLICATE, 0);
    job_put(j);     /* the handle's reference keeps it; with no handle, it goes */
    return h;
}

int syscall_job_set_limits(int32_t h, uint64_t limits) {
    /* Unprivileged: the handle's HRIGHT_SET_LIMITS is the authority. */
    struct job_limits l;
    int rc = job_limits_in(limits, &l);
    if (rc != E_OK) return rc;
    struct job *j = job_from_handle(h, HRIGHT_SET_LIMITS, &rc);
    if (!j) return rc;
    job_set_limits(j, l.memory, l.threads);
    job_put(j);
    return E_OK;
}

int syscall_job_kill(int32_t h) {
    /* Unprivileged: the handle's HRIGHT_KILL is the authority. */
    int rc;
    struct job *j = job_from_handle(h, HRIGHT_KILL, &rc);
    if (!j) return rc;
    if (!j->killed)
        klog_u32("USERMODE", SEV_INFO, "job killed by pid ", (uint32_t)thread_current_pid(), LOG_COLOR_VALUE, "");
    job_kill(j, thread_current_pid());
    job_put(j);
    return E_OK;
}

/* The gate's last step, and an interrupt's from ring 3 (the hook below).
   Nothing of the process's is held here: whatever the call took, it has let
   go of by now. */
void usermode_kill_point(void) {
    if (thread_killed())
        return_to_kernel(USERMODE_KILLED, thread_current_usave());   /* never returns */
}

/* Called from the system-call gate (arch/usermode.h), and by the self-tests,
   with the five argument registers as ring 3 left them. Every call passes the
   recording point on its way in and on its way out (sysrec.h). A number past
   the table, or in one of its holes, is E_NOT_SUPPORTED. */
int syscall_dispatch(uint32_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                     uintptr_t a4, uintptr_t a5) {
    const uintptr_t args[SYSCALL_ARGS] = { a1, a2, a3, a4, a5 };
    sysrec_enter(num, args);
    atomic_fetch_add(&dispatched, 1);
    syscall_decoder d = num < SYSCALL_TABLE_SIZE ? syscall_table[num] : 0;
    int rc = d ? d(args) : E_NOT_SUPPORTED;
    sysrec_exit(num, rc);
    return rc;
}

/* Only the system page calls the kernel (roadmap step 6a3c; Fuchsia's vDSO
   rule, OpenBSD's pinsyscall). The return address is the CPU's, pushed by
   int 0x80 or put in rcx by syscall, so ring 3 cannot forge it: a trap
   anywhere but in a stub, or a jump into a stub's trap with another number
   in eax, comes back to some other address. The number is bounded first, so
   num * SYSPAGE_STUB_SIZE cannot wrap onto a real stub.
   Every process's page is at SYSPAGE_VIRT today; when the loader picks the
   address per process, this reads the process's. */
static atomic_u32 refused;

uint32_t syscall_refused_count(void) { return atomic_load(&refused); }

void syscall_check_origin(uint32_t num, uintptr_t ip) {
    if (num < SYSCALL_TABLE_SIZE &&
        ip == SYSPAGE_VIRT + num * SYSPAGE_STUB_SIZE + arch_syscall_stub_ret())
        return;

    atomic_fetch_add(&refused, 1);
    int pid = thread_current_pid();
    klog_u32("USERMODE", SEV_ERR, "user process ", (uint32_t)pid, LOG_COLOR_VALUE,
             " called the kernel from outside the system page");
    char line[64];
    size_t n = 0;
    const char *sn = "syscall 0x", *at = " from 0x";
    while (*sn) line[n++] = *sn++;
    n += fmt_hex(line + n, num, 0, 0);
    while (*at) line[n++] = *at++;
    n += fmt_hex(line + n, (uint32_t)ip, 8, 0);
    line[n] = 0;
    klog_child(line);
    klog_child("process terminated");

    char rec[96];
    size_t m = 0;
    const char *name = thread_name(thread_current_id());
    #define REC_PUT(s) do { const char *p_ = (s); while (*p_ && m < sizeof rec - 1) rec[m++] = *p_++; } while (0)
    char num_s[12];
    REC_PUT("pid ");
    fmt_u32(num_s, (uint32_t)pid); REC_PUT(num_s);
    if (name) { REC_PUT(" ("); REC_PUT(name); REC_PUT(")"); }
    REC_PUT(" refused ");
    REC_PUT(line);
    #undef REC_PUT
    rec[m] = 0;
    crashlog_note_fault(rec);

    proc_note_crash(thread_proc(thread_current_id()), PROC_VECTOR_REFUSED, ip, 0);
    return_to_kernel(USERMODE_FAULTED, thread_current_usave());   /* never returns */
}

/* The system page of the kernel's own space, for the ring-3 helpers and
   tests that run in it (usermode_test, usermode_run_blob,
   process_create_ring3): the gate takes a call only from a stub, so they
   need one too. Several helpers run in the space at once, so it is counted:
   the first to enter ring 3 maps it and the last to leave unmaps it, and
   between runs the space's user half is empty again (x86-64's t_kernel_half
   holds it to that). A process with a space of its own maps its own
   (spawn.c). Interrupts are off around the count, so a helper preempted
   between the count and the map cannot be overtaken by another. */
static uint32_t ksys_users;
static paddr_t  ksys_phys;

static int ksyspage_get(void) {
    uint32_t f = arch_irq_save();
    int rc = ksys_users ? 0 : syspage_map(SYSPAGE_VIRT, &ksys_phys);
    if (rc == 0) ksys_users++;
    arch_irq_restore(f);
    return rc;
}

static void ksyspage_put(void) {
    uint32_t f = arch_irq_save();
    if (--ksys_users == 0) {
        unmap_user_page(SYSPAGE_VIRT, ksys_phys);
        ksys_phys = 0;
    }
    arch_irq_restore(f);
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
    uint64_t ustack_top = TEST_STACK_VIRT + 0x1000 - 16;
    *(uint64_t *)user_ptr(ustack_top) = user_addr(umsg);

    /* dev test runs as a SYSTEM ring-3 helper: grant it console so the gated
       SYS_WRITE works (restored after). */
    int      tme  = thread_current_id();
    uint32_t tsav = thread_current_caps();
    thread_set_caps(tme, GRANT_CONSOLE);
    /* enter ring 3; returns when the routine SYS_EXITs */
    int rc = -1;
    if (sealed && ksyspage_get() == 0) {
        rc = enter_usermode(TEST_CODE_VIRT, ustack_top, thread_current_usave(), SYSPAGE_VIRT);
        ksyspage_put();
    }
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

    /* The blob starts as a process does: the system page mapped (the kernel
       space's, ksyspage_get), its base in the register the ABI names. */
    int rc = -1;
    if (seal_user_code(TEST_CODE_VIRT, code_phys) == 0 && ksyspage_get() == 0) {
        rc = enter_usermode(TEST_CODE_VIRT, TEST_STACK_VIRT + 0x1000 - 16,
                            thread_current_usave(), SYSPAGE_VIRT);
        ksyspage_put();
    }

    unmap_user_page(TEST_CODE_VIRT, code_phys);
    unmap_user_page(TEST_STACK_VIRT, stack_phys);
    return rc;
}

/* ---- the system page (roadmap step 6a3b) ---- */
extern uint8_t user_sysp_call_start[],  user_sysp_call_end[];
extern uint8_t user_sysp_write_start[], user_sysp_write_end[];

static uint8_t sysp_probe[4096];

int usermode_syspage_test(void) {
    /* A stub in the slot of every syscall the table has, and int3 filling
       every other slot and the rest of the page. */
    syspage_fill(sysp_probe);
    uint32_t stubs = 0;
    int ok = 1;
    for (uint32_t n = 0; n < 4096u / SYSPAGE_STUB_SIZE; n++) {
        const uint8_t *slot = sysp_probe + n * SYSPAGE_STUB_SIZE;
        uint32_t traps = 0;
        for (uint32_t i = 0; i < SYSPAGE_STUB_SIZE; i++) traps += slot[i] == 0xCC;
        int has = n < SYSCALL_TABLE_SIZE && syscall_table[n];
        if (has) { stubs++; ok = ok && traps < SYSPAGE_STUB_SIZE; }
        else     ok = ok && traps == SYSPAGE_STUB_SIZE;
    }
    ok = ok && stubs == SYSCALL_COUNT;

    /* Ring 3 calls through the base it was handed: the SYS_GETPID stub gives
       this thread's pid, the SYS_EXIT stub ends the routine with it. */
    int rc = usermode_run_blob(user_sysp_call_start,
                               (uint32_t)(user_sysp_call_end - user_sysp_call_start));
    ok = ok && rc == thread_current_pid() + 0x100;

    /* And the page is code, never data: a store to it faults. */
    uint32_t faults = usermode_faults();
    rc = usermode_run_blob(user_sysp_write_start,
                           (uint32_t)(user_sysp_write_end - user_sysp_write_start));
    ok = ok && rc == USERMODE_FAULTED && usermode_faults() == faults + 1;

    /* And the kernel takes a call from nowhere else (6a3c). A SYS_EXIT(0)
       trapped directly from the blob, a SYS_GETPID made by jumping into the
       SYS_EXIT stub's trap, and the same jump with a number whose * 16 wraps
       to 0, are each refused: the process ends faulted, counted as a refusal
       and not as a fault, and no call is dispatched. Accepted, the first
       would exit 0; the others would return to the stub's ret and end on
       the blob's ud2, a fault. */
    extern uint8_t user_sysp_direct_start[], user_sysp_direct_end[];
    extern uint8_t user_sysp_midstub_start[], user_sysp_midstub_end[];
    extern uint8_t user_sysp_wrap_start[], user_sysp_wrap_end[];
    const uint8_t *bad[3][2] = {
        { user_sysp_direct_start,  user_sysp_direct_end },
        { user_sysp_midstub_start, user_sysp_midstub_end },
        { user_sysp_wrap_start,    user_sysp_wrap_end },
    };
    for (int i = 0; i < 3; i++) {
        uint32_t r0 = syscall_refused_count(), f0 = usermode_faults();
        uint32_t d0 = syscall_dispatch_count();
        rc = usermode_run_blob(bad[i][0], (uint32_t)(bad[i][1] - bad[i][0]));
        ok = ok && rc == USERMODE_FAULTED && syscall_refused_count() == r0 + 1
                && usermode_faults() == f0 && syscall_dispatch_count() == d0;
    }
    return ok;
}


/* One call through the dispatcher, each argument in the registers the ABI
   gives it: bit i of `wide` marks argument i a u64 or i64, which takes a pair
   (low, high) on x86-32 and one register on x86-64 (systable.h). The tests
   name arguments, not registers, so a decoder joining the wrong pair fails. */
static int um_call(uint32_t nr, uint32_t wide, uint64_t a0, uint64_t a1,
                   uint64_t a2, uint64_t a3, uint64_t a4) {
    const uint64_t in[5] = { a0, a1, a2, a3, a4 };
    uintptr_t r[SYSCALL_ARGS] = { 0 };
    int n = 0;
    for (int i = 0; i < 5 && n < SYSCALL_ARGS; i++) {
        r[n++] = (uintptr_t)in[i];
        if (SYSCALL_PAIRS && ((wide >> i) & 1u) && n < SYSCALL_ARGS)
            r[n++] = (uintptr_t)(in[i] >> 32);
    }
    return syscall_dispatch(nr, r[0], r[1], r[2], r[3], r[4]);
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
    /* a length past 4 GB is refused, not cut to its low half (12) */
    step = 4; if (um_call(SYS_CONSOLE_WRITE, 1u << 1, user_addr(line), (1ull << 32) | 12u, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
    step = 4; if (slog_total() != t0 + 1) goto done;

    /* 5: what is left without a newline waits, and a flush records it */
    proc_out_flush(p);
    e = slog_get(slog_count() - 1);
    step = 5; if (slog_total() != t0 + 2 || !lt_streq(e->event, "half", SLOG_EVT_W)) goto done;

    /* 6: a child inherits it, asked for or not */
    struct proc *c = proc_create(p, 0);
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
 * Drives SYS_FILE_* the way ring 3 does, short of the int 0x80 gate itself
 * (which the other syscalls already prove). The point of doing it from here
 * rather than from ktest.c is map_user_page: the args and buffers have to live
 * in a user mapping or user_ptr_readable rejects them, which is exactly the
 * check that would otherwise go untested until a real program tripped it.
 *
 * Goes through syscall_dispatch rather than calling sys_file_* directly, so
 * the GRANT_DISK gate is on the path too.
 */
#define FT_PATH  (TEST_CODE_VIRT + 0x100)
#define FT_PATH2 (TEST_CODE_VIRT + 0x180)
#define FT_DATA  (TEST_CODE_VIRT + 0x200)
#define FT_STAT  (TEST_CODE_VIRT + 0x400)
#define FT_INFO  (TEST_CODE_VIRT + 0x600)
#define FT_IARGS (TEST_CODE_VIRT + 0x680)
#define FT_POS   (TEST_CODE_VIRT + 0x6C0)   /* SEEK and TELL's *u64 */
#define FT_LARGS (TEST_CODE_VIRT + 0x6E0)   /* LOCK's file_lock_args */

/* SYS_OBJECT_INFO on `h`, its answer at FT_INFO. */
static int oi_call(int h, uint32_t topic, uint64_t cap) {
    struct object_info_args *a = (struct object_info_args *)FT_IARGS;
    a->handle   = h;
    a->topic    = topic;
    a->buf      = FT_INFO;
    a->cap      = cap;
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

/* One SYS_FILE_* call, its arguments in the registers abi/kernel.xfil gives
   them: the tests name every argument the same way for every call, and this
   places them, so a handler reading the wrong register fails its test. RENAME's
   new name is always FT_PATH2; SEEK and TELL's offset goes to FT_POS (ft_pos);
   LOCK's range and mode go in a file_lock_args at FT_LARGS. */
static int ft_call(uint32_t nr, int handle, uint32_t path, uint32_t data,
                   uint64_t len, int64_t off, uint32_t flags) {
    uint64_t h = (uint32_t)handle, o = (uint64_t)off;
    const uint32_t W1 = 1u << 1, W2 = 1u << 2;
    switch (nr) {
        case SYS_FILE_OPEN:     return um_call(nr, 0, path, flags, 0, 0, 0);
        case SYS_FILE_CLOSE:    return um_call(nr, 0, h, 0, 0, 0, 0);
        case SYS_FILE_TELL:
            *(volatile uint64_t *)FT_POS = 0xDEADBEEFDEADBEEFull;   /* must be overwritten */
            return um_call(nr, 0, h, FT_POS, 0, 0, 0);
        case SYS_FILE_READ:
        case SYS_FILE_WRITE:    return um_call(nr, W2, h, data, len, 0, 0);
        case SYS_FILE_SEEK:
            *(volatile uint64_t *)FT_POS = 0xDEADBEEFDEADBEEFull;
            return um_call(nr, W1, h, o, flags, FT_POS, 0);
        case SYS_FILE_TRUNCATE: return um_call(nr, W1, h, o, 0, 0, 0);
        case SYS_FILE_STAT:     return um_call(nr, 0, path, data, 0, 0, 0);
        case SYS_FILE_FSTAT:    return um_call(nr, 0, h, data, 0, 0, 0);
        case SYS_FILE_READDIR:  return um_call(nr, 0, path, len, data, 0, 0);
        case SYS_FILE_MKDIR:
        case SYS_FILE_UNLINK:
        case SYS_FILE_CHDIR:    return um_call(nr, 0, path, 0, 0, 0, 0);
        case SYS_FILE_RENAME:   return um_call(nr, 0, path, FT_PATH2, 0, 0, 0);
        case SYS_FILE_GETCWD:   return um_call(nr, W1, data, len, 0, 0, 0);
        case SYS_FILE_SYNC:     return um_call(nr, 0, 0, 0, 0, 0, 0);
        case SYS_FILE_LOCK: {
            struct file_lock_args *a = (struct file_lock_args *)FT_LARGS;
            a->start = o; a->len = len; a->mode = flags; a->reserved = 0;
            return um_call(nr, 0, h, FT_LARGS, 0, 0, 0);
        }
        case SYS_FILE_UNLOCK:   return um_call(nr, W1 | W2, h, o, len, 0, 0);
        default:                return E_INTERNAL;      /* not a file call: a test bug */
    }
}

/* SEEK and TELL answer 0 and leave the offset at FT_POS: the offset, or the
   error; any other success value is a failure of its own (E_INTERNAL). */
static int64_t ft_pos(int rc) {
    if (rc != E_OK) return rc < 0 ? rc : E_INTERNAL;
    return (int64_t)*(volatile uint64_t *)FT_POS;
}
#define ft_tell(h)            ft_pos(ft_call(SYS_FILE_TELL, (h), 0, 0, 0, 0, 0))
#define ft_seek(h, off, from) ft_pos(ft_call(SYS_FILE_SEEK, (h), 0, 0, 0, (off), (from)))

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
    int h = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0,
                    FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
    step++;
    if (h < 0) goto done;
    step++;
    if (sysfile_open_count() != base + 1) goto done;

    ft_str(FT_DATA, "hello world");
    step++;
    if (ft_call(SYS_FILE_WRITE, h, 0, FT_DATA, 11, 0, 0) != 11) goto done;
    step++;
    if (ft_tell(h) != 11) goto done;

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
    /* a u64 cap (ABI 10) is compared whole: 2^32 is room enough, not 0 */
    step++;
    if (oi_call(d, OBJINFO_BASIC, 1ull << 32) != ob) goto done;
    /* A user address is 64 bits (ABI 12) and is checked whole: FT_INFO plus
       4 GB is unmapped, and refused - cut to 32 bits it would be FT_INFO, and
       the answer would land there. */
    struct object_info_args *oia = (struct object_info_args *)FT_IARGS;
    oia->buf = FT_INFO + (1ull << 32);
    step++;
    if (syscall_dispatch(SYS_OBJECT_INFO, FT_IARGS, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    /* Padding is a field that must be 0, in each structure that has it; each
       is checked before the handle, so a dead handle tells the two apart. */
    struct ipc_call_args *ica = (struct ipc_call_args *)FT_IARGS;
    ica->req_len = 0; ica->reply_cap = 0; ica->ep_handle = -1; ica->req = 0; ica->reply = 0;
    ica->reserved = 0;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    ica->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    struct ipc_recv_args *ira = (struct ipc_recv_args *)FT_IARGS;
    ira->cap = 0; ira->ep_handle = -1; ira->buf = 0; ira->sender = 0;
    ira->reserved = 0;
    step++;
    if (syscall_dispatch(SYS_IPC_RECV, FT_IARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    ira->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_RECV, FT_IARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    struct ipc_reply_args *ipa = (struct ipc_reply_args *)FT_IARGS;
    ipa->data = 0; ipa->len = 0; ipa->ep_handle = -1;
    ipa->reserved = 0;
    step++;
    if (syscall_dispatch(SYS_IPC_REPLY, FT_IARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    ipa->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_REPLY, FT_IARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    step++;
    if (ft_call(SYS_FILE_WRITE, d, 0, FT_DATA, 1, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (ft_call(SYS_FILE_TRUNCATE, d, 0, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (ft_tell(d) != 11) goto done;
    step++;
    if (syscall_dispatch(SYS_HANDLE_CLOSE, (uint32_t)d, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (sysfile_open_count() != base + 1) goto done;  /* h still holds it */
    step++;
    if (ft_tell(d) != E_BAD_HANDLE) goto done;
    step++;
    if (oi_call(d, OBJINFO_BASIC, sizeof(struct object_basic)) != E_BAD_HANDLE) goto done;
    step++;
    if (ft_tell(h) != 11) goto done;

    step++;
    if (ft_seek(h, 0, FSEEK_SET) != 0) goto done;
    for (int i = 0; i < 16; i++) data[i] = '?';
    step++;
    if (ft_call(SYS_FILE_READ, h, 0, FT_DATA, 11, 0, 0) != 11) goto done;
    step++;
    if (data[0] != 'h' || data[4] != 'o' || data[6] != 'w' || data[10] != 'd') goto done;

    /* seek from the end, and read the tail */
    step++;
    if (ft_seek(h, -5, FSEEK_END) != 6) goto done;
    step++;
    if (ft_call(SYS_FILE_READ, h, 0, FT_DATA, 5, 0, 0) != 5) goto done;
    step++;
    if (data[0] != 'w' || data[4] != 'd') goto done;
    /* and the read past the end that follows it comes back empty, not short */
    step++;
    if (ft_call(SYS_FILE_READ, h, 0, FT_DATA, 5, 0, 0) != 0) goto done;

    /* --- u64 offsets and sizes (ABI 10). Each value has its high half set, so
       a decoder that dropped or swapped a register pair answers differently.
       5 GB is past anything 32 bits could hold; seeking there allocates
       nothing, and nothing here grows the file. --- */
    const uint64_t G5 = 5ull << 30;
    step++;
    if (ft_seek(h, (int64_t)G5, FSEEK_SET) != (int64_t)G5) goto done;
    step++;
    if (ft_tell(h) != (int64_t)G5) goto done;
    /* a negative i64 joined whole: 5 GB - 4 GB is 1 GB */
    step++;
    if (ft_seek(h, -(int64_t)(1ull << 32), FSEEK_CUR) != (int64_t)(1ull << 30)) goto done;
    /* before the start is refused, and the offset stays */
    step++;
    if (ft_seek(h, -(int64_t)G5, FSEEK_CUR) != E_OUT_OF_RANGE) goto done;
    step++;
    if (ft_tell(h) != (int64_t)(1ull << 30)) goto done;
    /* a pos the kernel cannot write is refused before the offset moves */
    step++;
    if (um_call(SYS_FILE_SEEK, 1u << 1, (uint32_t)h, 0, FSEEK_SET, 0xC0001000u, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (ft_tell(h) != (int64_t)(1ull << 30)) goto done;
    /* pos 0 is allowed: the offset moves and nothing is written */
    step++;
    if (um_call(SYS_FILE_SEEK, 1u << 1, (uint32_t)h, 6, FSEEK_SET, 0, 0) != E_OK) goto done;
    step++;
    if (ft_tell(h) != 6) goto done;
    /* a count past 32 bits is not its low half: 2^32 + 5 reads at most
       2^31 - 1 bytes, whose buffer is not mapped, rather than reading 5 */
    step++;
    if (ft_call(SYS_FILE_READ, h, 0, FT_DATA, (1ull << 32) + 5, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (ft_call(SYS_FILE_WRITE, h, 0, FT_DATA, (1ull << 32) + 5, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (ft_tell(h) != 6) goto done;
    /* a write that would carry the offset past INT64_MAX is refused (the
       store would refuse it too, but as E_INVALID_ARGS) */
    step++;
    if (ft_seek(h, INT64_MAX - 2, FSEEK_SET) != INT64_MAX - 2) goto done;
    if (ft_call(SYS_FILE_WRITE, h, 0, FT_DATA, 5, 0, 0) != E_OUT_OF_RANGE) goto done;
    /* a size past FILE_OFF_MAX, whose low half alone would be a fine size */
    step++;
    if (ft_call(SYS_FILE_TRUNCATE, h, 0, 0, 0, (int64_t)((1ull << 63) + 5), 0) != E_OUT_OF_RANGE) goto done;
    step++;
    if (ft_seek(h, 0, FSEEK_END) != 11) goto done;   /* back where the tests below expect */


    /* fstat agrees about the size */
    step++;
    if (ft_call(SYS_FILE_FSTAT, h, 0, FT_STAT, 0, 0, 0) != E_OK) goto done;
    struct file_stat *st = (struct file_stat *)FT_STAT;
    step++;
    if (st->size != 11 || st->kind != FTYPE_FILE) goto done;

    step++;
    if (ft_call(SYS_FILE_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (sysfile_open_count() != base) goto done;      /* the slot came back */
    step++;
    if (ft_call(SYS_FILE_READ, h, 0, FT_DATA, 4, 0, 0) != E_BAD_HANDLE) goto done;

    /* --- append opens at the end, and does not clobber --- */
    h = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_APPEND);
    step++;
    if (h < 0) goto done;
    ft_str(FT_DATA, "!!");
    step++;
    if (ft_call(SYS_FILE_WRITE, h, 0, FT_DATA, 2, 0, 0) != 2) goto done;
    step++;
    if (ft_call(SYS_FILE_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;

    h = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;
    if (h < 0) goto done;
    step++;
    if (ft_call(SYS_FILE_READ, h, 0, FT_DATA, 32, 0, 0) != 13) goto done;
    step++;
    if (data[10] != 'd' || data[11] != '!' || data[12] != '!') goto done;
    /* a read-only handle must refuse a write */
    step++;
    if (ft_call(SYS_FILE_WRITE, h, 0, FT_DATA, 2, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (ft_call(SYS_FILE_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;

    /* --- truncate cuts to the size given, and refuses a negative one;
       sync commits it --- */
    h = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE);
    step++;
    if (h < 0) goto done;
    step++;
    if (ft_call(SYS_FILE_TRUNCATE, h, 0, 0, 0, 5, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_FSTAT, h, 0, FT_STAT, 0, 0, 0) != E_OK || st->size != 5) goto done;
    step++;
    if (ft_call(SYS_FILE_TRUNCATE, h, 0, 0, 0, -1, 0) != E_OUT_OF_RANGE) goto done;   /* 2^64 - 1 */
    /* and back out to 13, so exec_path below meets the file it always has */
    step++;
    if (ft_call(SYS_FILE_TRUNCATE, h, 0, 0, 0, 13, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_FSTAT, h, 0, FT_STAT, 0, 0, 0) != E_OK || st->size != 13) goto done;
    step++;
    if (ft_call(SYS_FILE_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_SYNC, 0, 0, 0, 0, 0, 0) != E_OK) goto done;

    /* --- a missing file is E_NOT_FOUND without FOPEN_CREATE --- */
    ft_strp(FT_PATH, scratch, "/kt_absent.txt");
    step++;
    if (ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ) != E_NOT_FOUND) goto done;

    /* --- a bad user pointer is caught, not dereferenced --- */
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (ft_call(SYS_FILE_STAT, 0, FT_PATH, 0xC0001000u, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (ft_call(SYS_FILE_OPEN, 0, 0xC0001000u, 0, 0, 0, FOPEN_READ) != E_BAD_ADDRESS) goto done;

    /* --- directories, cwd, and relative paths --- */
    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (ft_call(SYS_FILE_MKDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_MKDIR, 0, FT_PATH, 0, 0, 0, 0) != E_ALREADY_EXISTS) goto done;
    step++;
    if (ft_call(SYS_FILE_CHDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    /* Compare the whole path against what we chdir'd to, terminator included,
       rather than spot-checking a few indices: the index checks silently
       stopped meaning anything the moment the scratch directory moved. */
    const char *want = ft_join(scratch, "/kt_dir");
    int wantlen = 0;
    while (want[wantlen]) wantlen++;
    step++;
    if (ft_call(SYS_FILE_GETCWD, 0, 0, FT_DATA, 64, 0, 0) != wantlen) goto done;
    /* a cap of 2^32 is a large cap, not a cap of 0 */
    step++;
    if (ft_call(SYS_FILE_GETCWD, 0, 0, FT_DATA, 1ull << 32, 0, 0) != wantlen) goto done;
    step++;
    for (int i = 0; i <= wantlen; i++) if (data[i] != want[i]) goto done;

    /* a bare name now resolves inside the new cwd, not at the root */
    ft_str(FT_PATH, "inner.txt");
    h = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0,
                FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
    step++;
    if (h < 0) goto done;
    step++;
    if (ft_call(SYS_FILE_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (cxfs_resolve(ft_join(scratch, "/kt_dir/inner.txt"), 0) < 0) goto done;   /* it really landed there */

    /* readdir finds it, and stops rather than repeating past the end */
    ft_str(FT_PATH, ".");
    step++;
    if (ft_call(SYS_FILE_READDIR, 0, FT_PATH, FT_STAT, 0, 0, 0) != 1) goto done;
    step++;
    if (st->name[0] != 'i' || st->kind != FTYPE_FILE) goto done;
    step++;
    if (ft_call(SYS_FILE_READDIR, 0, FT_PATH, FT_STAT, 1, 0, 0) != 0) goto done;

    /* rename, then a non-empty directory refuses to be removed */
    ft_str(FT_PATH2, "renamed.txt");
    step++;
    if (ft_call(SYS_FILE_RENAME, 0, FT_PATH, 0, 0, 0, 0) == E_OK) goto done;  /* "." is not renameable */
    ft_str(FT_PATH, "inner.txt");
    step++;
    if (ft_call(SYS_FILE_RENAME, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (cxfs_resolve(ft_join(scratch, "/kt_dir/renamed.txt"), 0) < 0) goto done;

    ft_str(FT_PATH, "/");
    step++;
    if (ft_call(SYS_FILE_CHDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (ft_call(SYS_FILE_UNLINK, 0, FT_PATH, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;  /* not empty */

    /* --- GRANT_DISK really is the gate --- */
    thread_set_caps(me, 0);
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (ft_call(SYS_FILE_STAT, 0, FT_PATH, FT_STAT, 0, 0, 0) != E_ACCESS_DENIED) goto done;
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
    sa->job = 0; sa->reserved = 0;

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
    if (syscall_dispatch(SYS_FILE_OPEN,     0xC0001000u, FOPEN_READ, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_CLOSE,    0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_READ,     0, 0xC0001000u, 1, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_WRITE,    0, 0xC0001000u, 1, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_SEEK,     0, 0, FSEEK_SET, 0, 0)   != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_TELL,     0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_TRUNCATE, 0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_STAT,     0xC0001000u, 0xC0001000u, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_FSTAT,    0, 0xC0001000u, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_READDIR,  0xC0001000u, 0, 0xC0001000u, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_MKDIR,    0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_UNLINK,   0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_RENAME,   0xC0001000u, 0xC0001000u, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_CHDIR,    0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_GETCWD,   0xC0001000u, 16, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_SYNC,     0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_FILE_LOCK,     0, 0, 0, FLOCK_SHARED, 0) != E_ACCESS_DENIED) goto done;   /* no FLOCK_WAIT: ungated, it could not block */
    step++;
    if (syscall_dispatch(SYS_FILE_UNLOCK,   0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
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
    if (syscall_dispatch(SYS_NET_STATUS,    0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_MAC,       0xC0001000u, 6, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_GET_CONFIG, 0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_SET_IP,    0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_SET_MASK,  0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_SET_GATEWAY, 0, 0, 0, 0, 0)         != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_SET_DNS,   0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_PING,      0, 0, 0, 0, 0)           != E_ACCESS_DENIED) goto done;   /* 0.0.0.0: refused at once if the gate were missing, no timeout */
    step++;
    if (syscall_dispatch(SYS_NET_SEND,      0xC0001000u, 0, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_RECV,      0xC0001000u, 1, 0, 0, 0) != E_ACCESS_DENIED) goto done;
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
    h = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
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
    uint64_t addr = 0;
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
    if (um_call(SYS_VM_UNMAP, 1u << 1, addr, (1ull << 32) | PAGE_SIZE, 0, 0, 0) != E_INVALID_ARGS) goto done;
    step++;
    if (syscall_dispatch(SYS_VM_UNMAP, addr, PAGE_SIZE, 0, 0, 0) != E_OK) goto done;
    addr = 0;
    step++;
    if (syscall_dispatch(SYS_VM_INFO, VT_USAGE, 0, 0, 0, 0) != E_OK || u->mapped != 0) goto done;

    /* padding must be 0 */
    a->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_VM_MAP, VT_ARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    a->reserved = 0;

    /* A hint is a whole address (ABI 12): one 4 GB above a free page in the
       window is outside it, so placement chooses. Cut to 32 bits it would be
       that free page, and honoured. */
    a->flags = MMAP_HINT;
    a->addr  = VM_MMAP_BASE + 0x100000ull + (1ull << 32);
    step++;
    if (syscall_dispatch(SYS_VM_MAP, VT_ARGS, 0, 0, 0, 0) != E_OK) goto done;
    addr = a->addr;
    step++;
    if (addr == VM_MMAP_BASE + 0x100000ull || addr < VM_MMAP_BASE || addr >= VM_MMAP_TOP) goto done;
    step++;
    if (syscall_dispatch(SYS_VM_UNMAP, addr, 0, 0, 0, 0) != E_OK) goto done;
    addr = 0;
    a->flags = 0;

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

/* ---- the network calls, through the dispatcher -----------------------------
 * The four SET calls get four different addresses, read back through
 * SYS_NET_GET_CONFIG and from the interface's own bytes (so a byte order that
 * is wrong both ways round is caught too), and the configuration is put back
 * after. PING is given only addresses it refuses at once, never one that waits
 * out the second-long timeout; a swapped dst and seq would ping 0.0.0.1 and
 * time out, which the test reads as a failure. SEND transmits one 60-byte
 * broadcast frame of a local-experimental ethertype. With no interface up,
 * PING's E_NOT_FOUND is asserted instead, and no frame is sent.
 */
#define NT_MAC   (TEST_CODE_VIRT + 0x000)
#define NT_CFG   (TEST_CODE_VIRT + 0x040)
#define NT_FRAME (TEST_CODE_VIRT + 0x100)
#define NT_RX    (TEST_CODE_VIRT + 0x200)
#define NT_FRAME_LEN 60
#define NT_RX_CAP    64

int usermode_net_test(void) {
    int me = thread_current_id();
    uint32_t save = thread_current_caps();
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;

    int ok = 0, step = 0, changed = 0;
    int ready = netif_ready();
    struct net_config old = *netif_cfg();
    thread_set_caps(me, GRANT_NET);

    step++;
    if (syscall_dispatch(SYS_NET_STATUS, 0, 0, 0, 0, 0) != (ready ? 1 : 0)) goto done;

    /* the MAC: six bytes, the seventh untouched; a short buffer and a kernel
       pointer refused */
    uint8_t *mac = (uint8_t *)user_ptr(NT_MAC);
    for (int i = 0; i < 8; i++) mac[i] = 0xEE;
    step++;
    if (syscall_dispatch(SYS_NET_MAC, NT_MAC, 5, 0, 0, 0) != E_BUFFER_TOO_SMALL) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_MAC, 0xC0001000u, 6, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    step++;
    if (um_call(SYS_NET_MAC, 1u << 1, NT_MAC, 1ull << 32, 0, 0, 0) != 6) goto done;   /* not 0, too small */
    if (syscall_dispatch(SYS_NET_MAC, NT_MAC, 8, 0, 0, 0) != 6) goto done;
    const uint8_t *m = netif_mac();
    step++;
    for (int i = 0; i < 6; i++) if (mac[i] != m[i]) goto done;
    if (mac[6] != 0xEE) goto done;

    /* four fields, four addresses */
    struct net_ip_config *c = (struct net_ip_config *)user_ptr(NT_CFG);
    step++;
    if (syscall_dispatch(SYS_NET_GET_CONFIG, 0xC0001000u, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    changed = 1;
    step++;
    if (syscall_dispatch(SYS_NET_SET_IP,      0x0A010203u, 0, 0, 0, 0) != E_OK) goto done;   /* 10.1.2.3 */
    if (syscall_dispatch(SYS_NET_SET_MASK,    0xFFFF0000u, 0, 0, 0, 0) != E_OK) goto done;
    if (syscall_dispatch(SYS_NET_SET_GATEWAY, 0x0A01FFFEu, 0, 0, 0, 0) != E_OK) goto done;
    if (syscall_dispatch(SYS_NET_SET_DNS,     0x08080404u, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_GET_CONFIG, NT_CFG, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (c->ip != 0x0A010203u || c->mask != 0xFFFF0000u ||
        c->gateway != 0x0A01FFFEu || c->dns != 0x08080404u) goto done;
    const struct net_config *now = netif_cfg();
    step++;
    if (now->ip[0] != 10 || now->ip[3] != 3 || now->mask[1] != 0xFF || now->mask[2] != 0) goto done;
    step++;
    for (int i = 0; i < 4; i++) if (now->dns2[i] != old.dns2[i]) goto done;   /* SET_DNS keeps the secondary */
    netif_set_ip(old.ip); netif_set_mask(old.mask);
    netif_set_gateway(old.gateway); netif_set_dns(old.dns1, old.dns2);
    changed = 0;

    /* ping: addresses refused before anything is sent */
    if (ready) {
        uint32_t ip = ((uint32_t)old.ip[0] << 24) | ((uint32_t)old.ip[1] << 16) |
                      ((uint32_t)old.ip[2] << 8) | (uint32_t)old.ip[3];
        uint32_t mask = ((uint32_t)old.mask[0] << 24) | ((uint32_t)old.mask[1] << 16) |
                        ((uint32_t)old.mask[2] << 8) | (uint32_t)old.mask[3];
        step++;
        if (syscall_dispatch(SYS_NET_PING, 0xFFFFFFFFu, 1, 0, 0, 0) != E_INVALID_ARGS) goto done;
        step++;
        if (syscall_dispatch(SYS_NET_PING, ip | ~mask, 1, 0, 0, 0) != E_INVALID_ARGS) goto done;
    } else {
        step++;
        if (syscall_dispatch(SYS_NET_PING, 0x0A000202u, 1, 0, 0, 0) != E_NOT_FOUND) goto done;
    }

    /* send: both sides of the length bound, a kernel pointer, then one frame */
    uint8_t *f = (uint8_t *)user_ptr(NT_FRAME);
    for (int i = 0; i < NT_FRAME_LEN; i++) f[i] = 0;
    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = m[i]; }
    f[12] = 0x88; f[13] = 0xB5;                    /* IEEE local experimental */
    step++;
    if (syscall_dispatch(SYS_NET_SEND, NT_FRAME, 0, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_SEND, NT_FRAME, 1515, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
    step++;
    if (um_call(SYS_NET_SEND, 1u << 1, NT_FRAME, (1ull << 32) | NT_FRAME_LEN, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_SEND, 0xC0001000u, NT_FRAME_LEN, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (ready && syscall_dispatch(SYS_NET_SEND, NT_FRAME, NT_FRAME_LEN, 0, 0, 0) != 0) goto done;

    /* receive: an empty buffer and a kernel pointer refused; a real one gets
       a frame or nothing, never more than it holds */
    step++;
    if (syscall_dispatch(SYS_NET_RECV, NT_RX, 0, 0, 0, 0) != E_BUFFER_TOO_SMALL) goto done;
    step++;
    /* cap 4 GB: its high half seen (a cap of 0 is too small) and clamped to
       64 KB, which is more than the one page mapped here */
    if (um_call(SYS_NET_RECV, 1u << 1, NT_RX, 1ull << 32, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    if (syscall_dispatch(SYS_NET_RECV, 0xC0001000u, NT_RX_CAP, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    step++;
    int r = syscall_dispatch(SYS_NET_RECV, NT_RX, NT_RX_CAP, 0, 0, 0);
    if (r < 0 || r > NT_RX_CAP) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "network syscall test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (changed) {
        netif_set_ip(old.ip); netif_set_mask(old.mask);
        netif_set_gateway(old.gateway); netif_set_dns(old.dns1, old.dns2);
    }
    thread_set_caps(me, save);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- advisory file locks ---------------------------------------------------
 * SYS_FILE_LOCK's contract, the parts a regression would break quietly:
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
 * The helper thread opens a path in its own part of the test page, so the
 * test thread rewriting FT_PATH cannot change what the helper opens.
 */
#define FL_PATH  (TEST_CODE_VIRT + 0x900)
#define FL_LARGS (TEST_CODE_VIRT + 0x980)   /* the helper's own file_lock_args */

static volatile int fl_stage;      /* how far the helper has got */
static volatile int fl_go;         /* set once its caps are in place */
static volatile int fl_rc;         /* what its waiting lock returned */

/* The helper's calls: OPEN of FL_PATH, LOCK and CLOSE (see ft_call). */
static int fl_call(uint32_t nr, int handle, uint64_t off, uint64_t len, uint32_t flags) {
    switch (nr) {
        case SYS_FILE_OPEN:  return syscall_dispatch(nr, FL_PATH, flags, 0, 0, 0);
        case SYS_FILE_LOCK: {
            struct file_lock_args *a = (struct file_lock_args *)FL_LARGS;
            a->start = off; a->len = len; a->mode = flags; a->reserved = 0;
            return syscall_dispatch(nr, (uint32_t)handle, FL_LARGS, 0, 0, 0);
        }
        case SYS_FILE_CLOSE: return syscall_dispatch(nr, (uint32_t)handle, 0, 0, 0, 0);
        default:             return E_INTERNAL;
    }
}

/* Holds [10,20), then waits for [0,10), which the test thread holds. */
static void fl_helper(void) {
    while (!fl_go) yield();               /* may run before thread_set_caps */
    int h = fl_call(SYS_FILE_OPEN, 0, 0, 0, FOPEN_READ);
    if (h < 0) { fl_rc = h; fl_stage = -1; return; }
    if (fl_call(SYS_FILE_LOCK, h, 10, 10, FLOCK_EXCLUSIVE) != E_OK) { fl_stage = -2; return; }
    fl_stage = 1;
    fl_rc = fl_call(SYS_FILE_LOCK, h, 0, 10, FLOCK_EXCLUSIVE | FLOCK_WAIT);
    fl_stage = 2;
    fl_call(SYS_FILE_CLOSE, h, 0, 0, 0);
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

    a = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
    b = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;                                                                       /* 1 */
    if (a < 0 || b < 0) goto done;

    /* --- whole file: shared with shared, exclusive with nothing --- */
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, FLOCK_SHARED) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 0, 0, FLOCK_SHARED) != E_OK) goto done;
    step++;                                                                       /* 4 */
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_BUSY) goto done;
    /* a refused conversion keeps the lock it had: a still conflicts with b */
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_BUSY) goto done;
    step++;
    if (ft_call(SYS_FILE_UNLOCK, a, 0, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 8 */
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, FLOCK_SHARED) != E_BUSY) goto done;
    /* waiting on a lock this same process holds could never end */
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, FLOCK_SHARED | FLOCK_WAIT) != E_DEADLOCK) goto done;

    /* advisory: a write through another handle is not refused */
    ft_str(FT_DATA, "locked..............");
    step++;                                                                       /* 10 */
    if (ft_call(SYS_FILE_WRITE, a, 0, FT_DATA, 20, 0, 0) != 20) goto done;

    /* closing b, with no UNLOCK, releases its lock */
    step++;
    if (ft_call(SYS_FILE_CLOSE, b, 0, 0, 0, 0, 0) != E_OK) goto done;
    closed = b;
    b = -1;
    step++;                                                                       /* 12 */
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;

    /* bad arguments */
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, 3) != E_INVALID_ARGS) goto done;
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, -1, FLOCK_SHARED) != E_OUT_OF_RANGE) goto done;   /* start 2^64 - 1 */
    step++;                                                                       /* 15 */
    if (ft_call(SYS_FILE_LOCK, closed, 0, 0, 0, 0, FLOCK_SHARED) != E_BAD_HANDLE) goto done;
    ft_call(SYS_FILE_UNLOCK, a, 0, 0, 0, 0, 0);

    /* --- byte ranges --- */
    b = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;
    if (b < 0) goto done;
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 10, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 18 */
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 10, 10, FLOCK_EXCLUSIVE) != E_OK) goto done;   /* disjoint */
    step++;
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 2, 9, FLOCK_SHARED) != E_BUSY) goto done;     /* overlaps a */
    /* an unlock inside a lock does not split it */
    step++;                                                                       /* 20 */
    if (ft_call(SYS_FILE_UNLOCK, a, 0, 0, 5, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 1, 0, FLOCK_SHARED) != E_BUSY) goto done;
    step++;
    if (ft_call(SYS_FILE_UNLOCK, a, 0, 0, 10, 0, 0) != E_OK) goto done;
    step++;                                                                       /* 23 */
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 1, 0, FLOCK_SHARED) != E_OK) goto done;
    /* len 0 runs to the end of the file, past anything written */
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 100, FLOCK_SHARED) != E_OK) goto done;
    step++;                                                                       /* 25 */
    if (ft_call(SYS_FILE_LOCK, b, 0, 0, 10, 5000, FLOCK_EXCLUSIVE) != E_BUSY) goto done;
    ft_call(SYS_FILE_UNLOCK, a, 0, 0, 0, 0, 0);
    ft_call(SYS_FILE_CLOSE, b, 0, 0, 0, 0, 0);
    b = -1;

    /* --- FLOCK_WAIT sleeps, a deadlock cycle is refused, a release wakes --- */
    step++;                                                                       /* 26 */
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 10, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
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
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 10, 10, FLOCK_EXCLUSIVE | FLOCK_WAIT) != E_DEADLOCK) goto done;
    step++;                                                                       /* 30 */
    if (ft_call(SYS_FILE_UNLOCK, a, 0, 0, 10, 0, 0) != E_OK) goto done;
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
    w = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ | FOPEN_WRITE);
    step++;
    if (w < 0) goto done;
    step++;                                                                       /* 34 */
    if (ft_call(SYS_FILE_WRITE, w, 0, FT_DATA, 6, 0, 0) != 6) goto done;

    /* --- unlinked while open: locked, still unlinkable, entry kept --- */
    step++;
    if (ft_call(SYS_FILE_LOCK, a, 0, 0, 0, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 36 */
    if (ft_call(SYS_FILE_UNLINK, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_STAT, 0, FT_PATH, FT_STAT, 0, 0, 0) != E_NOT_FOUND) goto done;
    step++;                                                                       /* 38 */
    if (ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ) != E_NOT_FOUND) goto done;
    step++;
    if (ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_CREATE) != E_ALREADY_EXISTS) goto done;
    /* the open handles still work */
    step++;                                                                       /* 40 */
    if (ft_seek(w, 0, FSEEK_SET) != 0) goto done;
    step++;
    if (ft_call(SYS_FILE_READ, w, 0, FT_DATA, 6, 0, 0) != 6) goto done;
    /* exec_path resolves for itself, and must not find it either (it would
       otherwise reach the signature check: E_INVALID_ARGS, not E_NOT_FOUND) */
    {
        struct spawn_args *sa = (struct spawn_args *)FT_STAT;
        sa->image = 0; sa->image_len = 0; sa->name = FT_PATH;
        sa->broker_endpoint = -1; sa->caps = 0;
        sa->job = 0; sa->reserved = 0;
        /* the arguments are checked before the path: FT_PATH's own string as
           the blob, 4 GB up, is unmapped (ABI 12) - cut to 32 bits it would be
           a good blob, and the call would go on to E_NOT_FOUND */
        uint32_t pl = 0;
        while (((const char *)user_ptr(FT_PATH))[pl]) pl++;
        sa->args = FT_PATH + (1ull << 32); sa->args_len = pl + 1;
        step++;                                                                   /* 42 */
        if (sys_exec_path(FT_PATH, user_addr(sa)) != E_BAD_ADDRESS) goto done;
        sa->args = 0; sa->args_len = 0;
        if (sys_exec_path(FT_PATH, user_addr(sa)) != E_NOT_FOUND) goto done;
    }
    /* and the id is not handed to the next file created */
    ft_strp(FT_PATH, scratch, "/kt_lock2.txt");
    other = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_CREATE);
    step++;                                                                       /* 43 */
    if (other < 0) goto done;
    ft_call(SYS_FILE_CLOSE, other, 0, 0, 0, 0, 0);
    step++;
    if (cxfs_resolve(ft_join(scratch, "/kt_lock2.txt"), 0) == id) goto done;
    /* the entry goes with the last close, not the first */
    ft_call(SYS_FILE_CLOSE, w, 0, 0, 0, 0, 0);
    w = -1;
    step++;                                                                       /* 45 */
    if (cxfs_read_entry((uint32_t)id, &e) != 0 || e.type != CXFS_TYPE_FILE) goto done;
    ft_call(SYS_FILE_CLOSE, a, 0, 0, 0, 0, 0);
    a = -1;
    step++;
    if (cxfs_read_entry((uint32_t)id, &e) == 0 && e.type != CXFS_TYPE_FREE) goto done;
    /* and the name is free again */
    ft_strp(FT_PATH, scratch, "/kt_lock.txt");
    w = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_WRITE | FOPEN_CREATE);
    step++;                                                                       /* 47 */
    if (w < 0) goto done;

    /* --- u64 ranges (ABI 10): a lock at 4 GB is not a lock at 0, through
       LOCK's file_lock_args and UNLOCK's register pairs alike --- */
    const uint64_t G4 = 1ull << 32;
    int p = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    int q = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;                                                                       /* 48 */
    if (p < 0 || q < 0) goto done;
    step++;
    if (ft_call(SYS_FILE_LOCK, p, 0, 0, 10, (int64_t)G4, FLOCK_EXCLUSIVE) != E_OK) goto done;
    step++;                                                                       /* 50 */
    if (ft_call(SYS_FILE_LOCK, q, 0, 0, 10, 0, FLOCK_EXCLUSIVE) != E_OK) goto done;   /* disjoint */
    step++;
    if (ft_call(SYS_FILE_LOCK, q, 0, 0, 1, (int64_t)G4 + 5, FLOCK_SHARED) != E_BUSY) goto done;
    step++;
    if (ft_call(SYS_FILE_UNLOCK, q, 0, 0, 10, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(SYS_FILE_UNLOCK, p, 0, 0, 10, (int64_t)G4, 0) != E_OK) goto done;
    step++;                                                                       /* 54 */
    if (ft_call(SYS_FILE_LOCK, q, 0, 0, 10, (int64_t)G4, FLOCK_EXCLUSIVE) != E_OK) goto done;   /* p's is gone */
    /* past FILE_OFF_MAX, in either half of the range */
    step++;
    if (ft_call(SYS_FILE_LOCK, p, 0, 0, 1ull << 63, 0, FLOCK_SHARED) != E_OUT_OF_RANGE) goto done;
    step++;
    if (ft_call(SYS_FILE_UNLOCK, p, 0, 0, 0, (int64_t)(1ull << 63), 0) != E_OUT_OF_RANGE) goto done;
    /* file_lock_args' padding must be 0 */
    struct file_lock_args *la = (struct file_lock_args *)FT_LARGS;
    la->start = 0; la->len = 0; la->mode = FLOCK_SHARED; la->reserved = 1;
    step++;                                                                       /* 57 */
    if (syscall_dispatch(SYS_FILE_LOCK, (uint32_t)p, FT_LARGS, 0, 0, 0) != E_INVALID_ARGS) goto done;

    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "file lock test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    htab_close_all(thread_htab(me));
    /* a helper still waiting is woken by the release above; let it finish */
    for (int i = 0; i < 50 && fl_stage > 0 && fl_stage < 3; i++) yield();
    ft_call(SYS_FILE_SYNC, 0, 0, 0, 0, 0, 0);                /* runs any deferred delete */
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

    uint64_t ustack_top = s->stack_virt + 0x1000 - 16;
    *(uint64_t *)user_ptr(ustack_top) = user_addr(umsg);

    if (sealed && ksyspage_get() == 0) {
        enter_usermode(s->code_virt, ustack_top, thread_current_usave(), SYSPAGE_VIRT);
        ksyspage_put();
    }

    /* user routine SYS_EXITed or faulted: save a fault noted on the way out
       (out of the exception now), reclaim its pages, leave the scheduler. */
    crashlog_save_faults();
    unmap_user_page(s->code_virt, s->code_phys);
    unmap_user_page(s->stack_virt, s->stack_phys);
    s->code_virt = 0;
    thread_exit();   /* never returns */
}

/* Everything but starting it, so a caller can finish configuring the process
   (its uid, say) before it can run. See thread_create_process: in `job`, or
   with NULL the creator's, whose refusal (E_CANCELED, E_NO_RESOURCES) is
   returned as it is. */
static int process_prepare_ring3(const char *name,
                                 const void *blob, uint32_t blob_len,
                                 const char *msg, struct job *job) {
    int pid = thread_create_process(name, process_trampoline, job);
    if (pid < 0) return pid;
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
    int pid = process_prepare_ring3(name, blob, blob_len, msg, 0);
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
    int pid = process_prepare_ring3(name, blob, blob_len, msg, 0);
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

    proc_note_crash(thread_proc(thread_current_id()), vector, (uint64_t)irq_frame_ip(r),
                    vector == 14 ? (uint64_t)irq_fault_address() : 0);
    return_to_kernel(USERMODE_FAULTED, thread_current_usave());   /* never returns */
}

void usermode_register_fault_handler(void) {
    set_user_fault_hook(usermode_fault);
    set_user_irq_return_hook(usermode_kill_point);
}
/* ---- process handles, waiting, how a process ended (Phase 1.1a) -----------
 * Real ring-3 processes, each with a process handle in this thread's table
 * made the way SYS_SPAWN makes one, driven through syscall_dispatch over a
 * user mapping. One exits with -42 after a 200 ms sleep, one executes ud2, one
 * is discarded before it runs. Then the records: alive while a handle is,
 * gone with the last one. 1 = pass, 0 = fail. */
extern uint8_t user_blob_exit_start[], user_blob_exit_end[];
extern uint8_t user64_ud2_start[], user64_ud2_end[];

#define PT_INFO  (TEST_CODE_VIRT + 0x100)   /* struct process_info / object_basic */
#define PT_IARGS (TEST_CODE_VIRT + 0x200)   /* struct object_info_args */
#define PT_SEEN  (TEST_CODE_VIRT + 0x300)   /* object_wait's *seen */

/* A held helper process in `job` (NULL: this thread's) and the caller's
   handle on it, as proc_start does it; *tid is its thread. The handle, or a
   negative error: the job's own refusal, if it made one. */
static int32_t pt_make_in(const uint8_t *start, const uint8_t *end, int *tid, struct job *job) {
    *tid = process_prepare_ring3("pt", start, (uint32_t)(end - start), "", job);
    if (*tid < 0) return *tid == -1 ? E_NO_MEMORY : *tid;
    int32_t h = htab_install(thread_htab(thread_current_id()), &thread_proc(*tid)->hdr,
                             HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_KILL | HRIGHT_STOP | HRIGHT_DUPLICATE, 0);
    if (h < 0) thread_discard(*tid);
    return h;
}

static int32_t pt_make(const uint8_t *start, const uint8_t *end, int *tid) {
    return pt_make_in(start, end, tid, 0);
}

/* OBJINFO_PROCESS on `h` into PT_INFO: the call's result. */
static int pt_info(int32_t h) {
    struct object_info_args *oi = (struct object_info_args *)user_ptr(PT_IARGS);
    oi->cap = sizeof(struct process_info);
    oi->buf = PT_INFO;
    oi->handle = h;
    oi->topic = OBJINFO_PROCESS;
    return syscall_dispatch(SYS_OBJECT_INFO, PT_IARGS, 0, 0, 0, 0);
}

/* SYS_OBJECT_WAIT `ms` from now: 0 only looks, PT_FOREVER has no deadline. */
#define PT_FOREVER 0xFFFFFFFFu
static int pt_wait(int32_t h, uint32_t sig, uint32_t ms, uint64_t seen) {
    uint64_t deadline = ms == PT_FOREVER ? DEADLINE_NEVER
                      : ms == 0          ? 0 : timer_now_ns() + (uint64_t)ms * TIMER_NS_PER_TICK;
    return syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)h, sig, deadline, seen, 0);
}

int usermode_process_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    volatile uint32_t *seen = (volatile uint32_t *)user_ptr(PT_SEEN);
    uint32_t live0 = proc_live(), faults0 = usermode_faults();
    int32_t h = -1, hc = -1, hn = -1, d = -1, eh = -1, ew = -1;
    int tid = -1, tc = -1, tn = -1, pid = -1, step = 0, ok = 0;

    /* 1-3: a handle on a running process, with the rights spawn gives */
    h = pt_make(user_blob_exit_start, user_blob_exit_end, &tid);
    step = 1; if (h <= 0) goto done;
    pid = thread_pid(tid);
    struct proc *p = thread_proc(tid);
    thread_start(tid);
    struct object_info_args *oi = (struct object_info_args *)user_ptr(PT_IARGS);
    struct object_basic *ob = (struct object_basic *)user_ptr(PT_INFO);
    oi->cap = sizeof *ob; oi->buf = PT_INFO; oi->handle = h; oi->topic = OBJINFO_BASIC;
    step = 2; if (syscall_dispatch(SYS_OBJECT_INFO, PT_IARGS, 0, 0, 0, 0) != (int)sizeof *ob
                  || ob->kind != OBJ_TYPE_PROCESS
                  || ob->rights != (HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_KILL | HRIGHT_STOP | HRIGHT_DUPLICATE)) goto done;
    step = 3; if (pt_info(h) != (int)sizeof *pi || pi->pid != (uint32_t)pid
                  || pi->ended != PROC_RUNNING) goto done;

    /* 4-5: still running: a look, and a short wait, both time out, and the
       short one did wait */
    *seen = 0xFFFFFFFFu;
    step = 4; if (pt_wait(h, SIGNAL_PROCESS_ENDED, 0, PT_SEEN) != E_TIMED_OUT || *seen != 0) goto done;
    uint32_t t0 = timer_ticks();
    step = 5; if (pt_wait(h, SIGNAL_PROCESS_ENDED, 20, 0) != E_TIMED_OUT
                  || timer_ticks() - t0 < 20 || kobj_waiters(&p->hdr) != 0) goto done;

    /* 6-10: refusals, each its own error */
    step = 6; if (pt_wait(h, 0, 0, 0) != E_INVALID_ARGS) goto done;
    step = 7; if (pt_wait(h, SIGNAL_PROCESS_ENDED << 1, 0, 0) != E_INVALID_ARGS) goto done;
    /* 8: a deadline already passed, not only 0, just looks (one tick
       ago: a wait for it would be one tick long) */
    uint64_t past = timer_now_ns() - TIMER_NS_PER_TICK;
    t0 = timer_ticks();
    step = 8; if (syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)h, SIGNAL_PROCESS_ENDED, past, 0, 0) != E_TIMED_OUT
                  || timer_ticks() - t0 > 1) goto done;
    step = 9; if (pt_wait(h, SIGNAL_PROCESS_ENDED, 0, USER_VLIMIT) != E_BAD_ADDRESS) goto done;
    step = 10; if (pt_wait(9999, SIGNAL_PROCESS_ENDED, 0, 0) != E_BAD_HANDLE) goto done;

    /* 11-12: each right is needed: no WAIT, no wait; no INSPECT, no info */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)h, HRIGHT_INSPECT, 0, 0, 0);
    step = 11; if (d <= 0 || pt_wait(d, SIGNAL_PROCESS_ENDED, 0, 0) != E_ACCESS_DENIED) goto done;
    htab_close(tab, d);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)h, HRIGHT_WAIT, 0, 0, 0);
    step = 12; if (d <= 0 || pt_info(d) != E_ACCESS_DENIED) goto done;

    /* 13-14: an endpoint is the wrong type for OBJINFO_PROCESS, and has no
       signals to wait for even on a handle that may wait */
    eh = ep_create();
    step = 13; if (eh <= 0 || pt_info(eh) != E_WRONG_TYPE) goto done;
    struct kobj *eo = htab_ref(tab, eh, KOBJ_ENDPOINT, 0, 0);
    ew = eo ? htab_install(tab, eo, HRIGHT_WAIT, 0) : -1;
    if (eo) kobj_unref(eo);
    step = 14; if (ew <= 0 || pt_wait(ew, SIGNAL_PROCESS_ENDED, 0, 0) != E_NOT_SUPPORTED) goto done;

    /* 15-16: waiting with no limit returns when it ends, and says how: SYS_EXIT
       and its code, whole. The pid stays its own while the handle is open. */
    *seen = 0;
    step = 15; if (pt_wait(d, SIGNAL_PROCESS_ENDED, PT_FOREVER, PT_SEEN) != E_OK
                   || *seen != SIGNAL_PROCESS_ENDED) goto done;
    step = 16; if (pt_info(h) != (int)sizeof *pi || pi->ended != PROC_EXITED || pi->code != -42
                   || pi->pid != (uint32_t)pid || proc_by_pid(pid) != p) goto done;

    /* 17: a fault: crashed, with the vector and where */
    hc = pt_make(user64_ud2_start, user64_ud2_end, &tc);
    uint64_t code_at = R3_BASE + (uint64_t)tc * R3_SLOT_SIZE;
    if (hc > 0) thread_start(tc);
    step = 17; if (hc <= 0 || pt_wait(hc, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || pt_info(hc) <= 0
                   || pi->ended != PROC_CRASHED || pi->vector != 6 || pi->ip != code_at
                   || usermode_faults() != faults0 + 1) goto done;

    /* 18: one that never ran ended too, and says so */
    hn = pt_make(user_blob_exit_start, user_blob_exit_end, &tn);
    if (hn > 0) thread_discard(tn);
    step = 18; if (hn <= 0 || pt_wait(hn, SIGNAL_PROCESS_ENDED, 0, 0) != E_OK || pt_info(hn) <= 0
                   || pi->ended != PROC_NOT_STARTED) goto done;

    /* 19: no zombies: each record goes with its last handle, and its pid */
    htab_close(tab, h);  h = -1;
    step = 19; if (proc_by_pid(pid) != p) goto done;   /* d still holds it */
    htab_close(tab, d);  d = -1;
    htab_close(tab, hc); hc = -1;
    htab_close(tab, hn); hn = -1;
    step = 19; if (proc_by_pid(pid) != 0 || proc_live() != live0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "process handle test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* A process still running is waited for, so none outlives the test. */
    if (h > 0)  { pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0);  htab_close(tab, h); }
    if (hc > 0) { pt_wait(hc, SIGNAL_PROCESS_ENDED, 3000, 0); htab_close(tab, hc); }
    if (hn > 0) htab_close(tab, hn);
    if (d > 0)  htab_close(tab, d);
    if (ew > 0) htab_close(tab, ew);
    if (eh > 0) htab_close(tab, eh);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- SYS_PROCESS_KILL (Phase 1.1b) ----
 * A ring-3 process in each kind of wait, killed: asleep, in an object wait,
 * receiving, calling before and after the receiver took the request, reading
 * the keyboard, waiting for a file lock. Then one spinning in ring 3 with
 * preemption on, whose safe point is an interrupt, and one that kills itself,
 * whose safe point is the gate. Each ends PROC_KILLED with the killer's pid
 * well inside the bound, and leaves nothing registered in the wait it was in.
 * Then the refusals, and a kill of a process that has ended. 1 = pass. */
extern uint8_t user_blob_call_start[], user_blob_call_end[];
extern uint8_t user_blob_loop_start[], user_blob_loop_end[];

#define KT_ARGS  (TEST_CODE_VIRT + 0x400)   /* the victim's ipc args */
#define KT_TARGS (TEST_CODE_VIRT + 0x480)   /* the test thread's ipc and lock args */
#define KT_LARGS (TEST_CODE_VIRT + 0x500)   /* the victim's file_lock_args */
#define KT_REQ   (TEST_CODE_VIRT + 0x580)   /* one request byte */
#define KT_PATH  (TEST_CODE_VIRT + 0x600)

#define KT_BLOB_MAX 96
/* user_blob_call and the call it makes: read when the victim starts, so one
   victim at a time. */
static uint8_t  kt_image[KT_BLOB_MAX + 6 * 8];
static uint32_t kt_len;

/* A held victim running user_blob_call, and the test's handle on it. */
static int32_t kt_make(int *tid) {
    kt_len = (uint32_t)(user_blob_call_end - user_blob_call_start);
    if (kt_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t i = 0; i < kt_len; i++) kt_image[i] = user_blob_call_start[i];
    return pt_make(kt_image, kt_image + kt_len + 6 * 8, tid);
}

/* The call it will make. */
static void kt_call(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    const uint64_t q[6] = { nr, a1, a2, a3, a4, a5 };
    const uint8_t *s = (const uint8_t *)q;
    for (uint32_t i = 0; i < sizeof q; i++) kt_image[kt_len + i] = s[i];
}

static int kt_kill_call(int32_t h) {
    return syscall_dispatch(SYS_PROCESS_KILL, (uint32_t)h, 0, 0, 0, 0);
}

/* A started victim is still there (blocked, if `blocked`), the kill is taken,
   and it ends killed by this thread's process, without running on in ring 3
   (user_blob_call's ud2 would be a fault). 1 = so. */
static int kt_kill(int32_t h, int tid, int blocked) {
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    uint32_t faults = usermode_faults();
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT) return 0;
    if (blocked && !thread_is_blocked(tid)) return 0;
    if (kt_kill_call(h) != E_OK) return 0;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK) return 0;
    return pt_info(h) == (int)sizeof *pi && pi->ended == PROC_KILLED
        && pi->killer == (uint32_t)thread_current_pid() && pi->reserved == 0
        && usermode_faults() == faults;
}

int usermode_kill_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    int me = thread_current_id();
    struct htab *tab = thread_htab(me);
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    uint32_t live0 = proc_live(), faults0 = usermode_faults(), refused0 = syscall_refused_count();
    uint32_t caps0 = thread_current_caps();
    struct endpoint *ep = 0;
    int32_t h = -1, v, eh = -1, d = -1, fa = -1, fb = -1;
    int tid = -1, step = 0, ok = 0, preempt = 0, of0 = 0, id;
    const char *scratch = "";
    struct proc *p;

    /* 1: asleep for the longest sleep there is */
    h = kt_make(&tid);
    step = 1; if (h <= 0) goto done;
    kt_call(SYS_SLEEP, SLEEP_MAX_MS, 0, 0, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1)) goto done;
    htab_close(tab, h); h = -1;

    /* 2: waiting on an object (its own process, which cannot end meanwhile),
       and off its waiter list after */
    h = kt_make(&tid);
    step = 2; if (h <= 0) goto done;
    p = thread_proc(tid);
    v = htab_install(thread_htab(tid), &p->hdr, HRIGHT_WAIT, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    kt_call(SYS_OBJECT_WAIT, (uint32_t)v, SIGNAL_PROCESS_ENDED, DEADLINE_NEVER, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1) || kobj_waiters(&p->hdr) != 0) goto done;
    htab_close(tab, h); h = -1;

    /* 3: receiving, and no longer named the endpoint's receiver */
    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    h = ep ? kt_make(&tid) : -1;
    step = 3; if (h <= 0) goto done;
    v = ep_install_handle(tid, ep, HRIGHT_RECV, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    struct ipc_recv_args *ra = (struct ipc_recv_args *)user_ptr(KT_ARGS);
    ra->cap = 0; ra->buf = 0; ra->sender = 0; ra->ep_handle = v; ra->reserved = 0;
    kt_call(SYS_IPC_RECV, KT_ARGS, 0, 0, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1) || ep->recv_tid != -1) goto done;
    htab_close(tab, h); h = -1;

    /* 4: calling, before anyone took the request: it is withdrawn */
    struct ipc_call_args *ca = (struct ipc_call_args *)user_ptr(KT_ARGS);
    *(volatile uint8_t *)user_ptr(KT_REQ) = 0x5A;
    h = kt_make(&tid);
    step = 4; if (h <= 0) goto done;
    v = ep_install_handle(tid, ep, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    ca->req_len = 1; ca->reply_cap = 0; ca->req = KT_REQ; ca->reply = 0;
    ca->ep_handle = v; ca->reserved = 0;
    kt_call(SYS_IPC_CALL, KT_ARGS, 0, 0, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1) || ep->caller_tid != -1) goto done;
    htab_close(tab, h); h = -1;

    /* 5-6: calling, with the request taken: the transaction stays until the
       reply, which finds the caller gone, and then the endpoint is free */
    h = kt_make(&tid);
    step = 5; if (h <= 0) goto done;
    v = ep_install_handle(tid, ep, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    ca->ep_handle = v;
    kt_call(SYS_IPC_CALL, KT_ARGS, 0, 0, 0, 0);
    thread_start(tid);
    struct ipc_recv_args *tr = (struct ipc_recv_args *)user_ptr(KT_TARGS);
    tr->cap = 0; tr->buf = 0; tr->sender = 0; tr->ep_handle = eh; tr->reserved = 0;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT
        || syscall_dispatch(SYS_IPC_RECV, KT_TARGS, 0, 0, 0, 0) != 0) goto done;
    if (!kt_kill(h, tid, 1) || ep->caller_tid == -1 || !ep->caller_gone) goto done;
    struct ipc_reply_args *rp = (struct ipc_reply_args *)user_ptr(KT_TARGS);
    rp->data = KT_REQ; rp->len = 1; rp->ep_handle = eh; rp->reserved = 0;
    step = 6; if (syscall_dispatch(SYS_IPC_REPLY, KT_TARGS, 0, 0, 0, 0) != E_PEER_CLOSED
                  || ep->caller_tid != -1 || ep->caller_gone
                  || syscall_dispatch(SYS_IPC_REPLY, KT_TARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    htab_close(tab, h); h = -1;

    /* 7: reading the keyboard */
    h = kt_make(&tid);
    step = 7; if (h <= 0) goto done;
    kt_call(SYS_INPUT_READ, 0, 0, 0, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1)) goto done;
    htab_close(tab, h); h = -1;

    /* 8-9: waiting for a file lock another open file holds: off the wait
       list, and its open file closed with it */
    if (cxfs_is_mounted()) {
        thread_set_caps(me, GRANT_DISK);
        scratch = (cxfs_resolve("/Temp", 0) >= 0) ? "/Temp" : "";
        ft_strp(KT_PATH, scratch, "/kt_kill.txt");
        of0 = sysfile_open_count();
        fa = syscall_dispatch(SYS_FILE_OPEN, KT_PATH, FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC, 0, 0, 0);
        fb = syscall_dispatch(SYS_FILE_OPEN, KT_PATH, FOPEN_READ, 0, 0, 0);
        struct file_lock_args *la = (struct file_lock_args *)user_ptr(KT_TARGS);
        la->start = 0; la->len = 0; la->mode = FLOCK_EXCLUSIVE; la->reserved = 0;
        step = 8; if (fa <= 0 || fb <= 0
                      || syscall_dispatch(SYS_FILE_LOCK, (uint32_t)fa, KT_TARGS, 0, 0, 0) != E_OK) goto done;
        h = kt_make(&tid);
        if (h <= 0) goto done;
        struct htab_info fi;
        struct kobj *fo = htab_ref(tab, fb, KOBJ_FILE, 0, &fi);
        v = fo ? htab_install(thread_htab(tid), fo, fi.rights, 0) : -1;
        if (fo) kobj_unref(fo);
        if (v <= 0) { thread_discard(tid); goto done; }
        htab_close(tab, fb); fb = -1;
        thread_set_caps(tid, GRANT_DISK);
        struct file_lock_args *vl = (struct file_lock_args *)user_ptr(KT_LARGS);
        vl->start = 0; vl->len = 0; vl->mode = FLOCK_EXCLUSIVE | FLOCK_WAIT; vl->reserved = 0;
        kt_call(SYS_FILE_LOCK, (uint32_t)v, KT_LARGS, 0, 0, 0);
        thread_start(tid);
        if (!kt_kill(h, tid, 1) || sysfile_lock_waiting() != 0) goto done;
        step = 9; if (sysfile_open_count() != of0 + 1) goto done;
        htab_close(tab, h); h = -1;
    }

    /* 10: spinning in ring 3, preemption on: an interrupt is the safe point */
    h = pt_make(user_blob_loop_start, user_blob_loop_end, &tid);
    step = 10; if (h <= 0) goto done;
    sched_preempt_enable(5); preempt = 1;
    thread_start(tid);
    if (!kt_kill(h, tid, 0)) goto done;
    sched_preempt_disable(); preempt = 0;
    htab_close(tab, h); h = -1;

    /* 11: one that kills itself leaves at the gate, its own pid the killer */
    h = kt_make(&tid);
    step = 11; if (h <= 0) goto done;
    int vpid = thread_pid(tid);
    v = htab_install(thread_htab(tid), &thread_proc(tid)->hdr, HRIGHT_KILL, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    kt_call(SYS_PROCESS_KILL, (uint32_t)v, 0, 0, 0, 0);
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || pt_info(h) <= 0
        || pi->ended != PROC_KILLED || pi->killer != (uint32_t)vpid
        || usermode_faults() != faults0) goto done;
    htab_close(tab, h); h = -1;

    /* 12-14: the refusals: no HRIGHT_KILL, not a process, no handle */
    h = pt_make(user_blob_exit_start, user_blob_exit_end, &tid);
    step = 12; if (h <= 0) goto done;
    thread_start(tid);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)h, HRIGHT_WAIT | HRIGHT_INSPECT, 0, 0, 0);
    if (d <= 0 || kt_kill_call(d) != E_ACCESS_DENIED) goto done;
    step = 13; if (kt_kill_call(eh) != E_WRONG_TYPE) goto done;
    step = 14; if (kt_kill_call(9999) != E_BAD_HANDLE) goto done;

    /* 15: a process that has ended: 0, and how it ended stands */
    step = 15; if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || kt_kill_call(h) != E_OK
                   || pt_info(h) <= 0 || pi->ended != PROC_EXITED || pi->code != -42
                   || pi->killer != 0) goto done;
    htab_close(tab, d); d = -1;
    htab_close(tab, h); h = -1;

    /* 16-17: killed before it started, then a wait: the kill's wakes are long
       past, so it must not block at all - a sleep (thread_block_prepare) and
       the keyboard (thread_block) */
    h = kt_make(&tid);
    step = 16; if (h <= 0) goto done;
    kt_call(SYS_SLEEP, SLEEP_MAX_MS, 0, 0, 0, 0);
    if (kt_kill_call(h) != E_OK) { thread_discard(tid); goto done; }
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || pt_info(h) <= 0
        || pi->ended != PROC_KILLED || usermode_faults() != faults0) goto done;
    htab_close(tab, h); h = -1;
    h = kt_make(&tid);
    step = 17; if (h <= 0) goto done;
    kt_call(SYS_INPUT_READ, 0, 0, 0, 0, 0);
    if (kt_kill_call(h) != E_OK) { thread_discard(tid); goto done; }
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || pt_info(h) <= 0
        || pi->ended != PROC_KILLED || usermode_faults() != faults0) goto done;
    htab_close(tab, h); h = -1;

    /* 18: nothing left behind, and nothing ended any other way */
    step = 18; if (proc_live() != live0 || usermode_faults() != faults0
                   || syscall_refused_count() != refused0) goto done;
    ok = 1;

done:
    if (preempt) sched_preempt_disable();
    if (!ok) klog_u32("KTEST", SEV_ERR, "kill test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* A victim still running is killed, so none outlives the test. */
    if (h > 0) { kt_kill_call(h); pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0); htab_close(tab, h); }
    if (d > 0)  htab_close(tab, d);
    if (fa > 0) htab_close(tab, fa);
    if (fb > 0) htab_close(tab, fb);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    if (cxfs_is_mounted() && (id = cxfs_resolve(ft_join(scratch, "/kt_kill.txt"), 0)) >= 0)
        cxfs_delete_entry((uint32_t)id);
    thread_set_caps(me, caps0);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- SYS_PROCESS_STOP (Phase 1.1c) ----
 * Registering a stop endpoint and its refusals; a process that is receiving
 * when asked, one that asks itself before it receives (so the notice waits
 * for it, and the stopper is a real pid), one that ignores it until the
 * deadline, a deadline brought closer and not put back, no endpoint and a
 * dead one (ended at once), a kill while a stop is pending, and a reply after
 * a notice, which answers nothing. 1 = pass. */
extern uint8_t user_blob_calls_start[], user_blob_calls_end[];

#define ST_NOTE   (TEST_CODE_VIRT + 0x680)   /* the victim's stop_notice */
#define ST_SENDER (TEST_CODE_VIRT + 0x6C0)   /* and who it came from */
#define ST_TBUF   (TEST_CODE_VIRT + 0x700)   /* the test thread's receive buffer */
#define ST_TSEND  (TEST_CODE_VIRT + 0x740)   /* and its sender */

/* user_blob_calls and its three calls, read when the victim starts. */
static uint8_t  st_image[KT_BLOB_MAX + 3 * 6 * 8];
static uint32_t st_len;

static int32_t st_make(int *tid) {
    st_len = (uint32_t)(user_blob_calls_end - user_blob_calls_start);
    if (st_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t i = 0; i < st_len; i++) st_image[i] = user_blob_calls_start[i];
    return pt_make(st_image, st_image + st_len + 3 * 6 * 8, tid);
}

/* Call `i` (0-2) of the three. */
static void st_call(uint32_t i, uint64_t nr, uint64_t a1, uint64_t a2) {
    const uint64_t q[6] = { nr, a1, a2, 0, 0, 0 };
    const uint8_t *s = (const uint8_t *)q;
    for (uint32_t k = 0; k < sizeof q; k++) st_image[st_len + i * sizeof q + k] = s[k];
}

static int st_stop(int32_t h, uint32_t ms) {
    return syscall_dispatch(SYS_PROCESS_STOP, (uint32_t)h, ms, 0, 0, 0);
}

static int st_register(int32_t h) {
    return syscall_dispatch(SYS_PROCESS_STOP_ENDPOINT, (uint32_t)h, 0, 0, 0, 0);
}

/* The test thread receives on `eh` with room for `cap` bytes. */
static int st_recv(int32_t eh, uint64_t cap) {
    struct ipc_recv_args *r = (struct ipc_recv_args *)user_ptr(KT_TARGS);
    r->cap = cap; r->buf = ST_TBUF; r->sender = ST_TSEND; r->ep_handle = eh; r->reserved = 0;
    return syscall_dispatch(SYS_IPC_RECV, KT_TARGS, 0, 0, 0, 0);
}

static int st_reply(int32_t eh) {
    struct ipc_reply_args *r = (struct ipc_reply_args *)user_ptr(KT_TARGS);
    r->data = KT_REQ; r->len = 1; r->ep_handle = eh; r->reserved = 0;
    return syscall_dispatch(SYS_IPC_REPLY, KT_TARGS, 0, 0, 0, 0);
}

/* What the test thread just received is a stop notice from `by` giving `ms`. */
static int st_is_notice(int rc, const void *buf, const void *sender, uint32_t by, uint32_t ms) {
    const struct stop_notice *n = buf;
    return rc == (int)sizeof *n && *(const int32_t *)sender == IPC_SENDER_KERNEL
        && n->kind == NOTICE_STOP && n->stopper == by && n->deadline_ms == ms
        && n->reserved == 0;
}

/* A victim that registers `v` (its handle on the endpoint) and then sleeps,
   twice, for the longest sleep there is: it never receives. */
static int32_t st_deaf(struct endpoint *ep, int *tid, int32_t *v) {
    int32_t h = st_make(tid);
    if (h <= 0) return h;
    *v = ep_install_handle(*tid, ep, HRIGHT_RECV, 0);
    if (*v <= 0) { thread_discard(*tid); return E_NO_RESOURCES; }
    st_call(0, SYS_PROCESS_STOP_ENDPOINT, (uint32_t)*v, 0);
    st_call(1, SYS_SLEEP, SLEEP_MAX_MS, 0);
    st_call(2, SYS_SLEEP, SLEEP_MAX_MS, 0);
    thread_start(*tid);
    return h;
}

/* `h` ends as `how` says, for this thread's process, without running on in
   ring 3. */
static int st_ends(int32_t h, uint32_t how) {
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    uint32_t faults = usermode_faults();
    return pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) == E_OK && pt_info(h) == (int)sizeof *pi
        && pi->ended == how && pi->killer == (uint32_t)thread_current_pid()
        && pi->reserved == 0 && usermode_faults() == faults;
}

int usermode_stop_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    int me = thread_current_id();
    struct htab *tab = thread_htab(me);
    struct proc *mine = thread_proc(me);
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    struct stop_notice *note = (struct stop_notice *)user_ptr(ST_NOTE);
    const void *tbuf = user_ptr(ST_TBUF), *tsend = user_ptr(ST_TSEND);
    uint32_t live0 = proc_live(), faults0 = usermode_faults(), refused0 = syscall_refused_count();
    int eps0 = ep_in_use_count();
    struct endpoint *ep = 0, *ep2 = 0;
    int32_t h = -1, ha = -1, v, eh = -1, e2 = -1, d = -1;
    int tid = -1, tida = -1, step = 0, ok = 0;
    uint32_t t0;

    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;

    /* 1: registering, here for the test's own process: the refusals, then
       one that takes, then none */
    step = 1; if (!ep) goto done;
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)eh, HRIGHT_DUPLICATE, 0, 0, 0);
    if (d <= 0 || st_register(d) != E_ACCESS_DENIED || st_register(9999) != E_BAD_HANDLE
        || mine->stop_ep != 0) goto done;
    if (st_register(eh) != E_OK || mine->stop_ep != ep) goto done;
    if (st_register(0) != E_OK || mine->stop_ep != 0) goto done;
    htab_close(tab, d); d = -1;

    /* 2: the stop's refusals, on a victim not yet started */
    h = st_make(&tid);
    step = 2; if (h <= 0) goto done;
    if (st_register(h) != E_WRONG_TYPE) goto done;
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)h, HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_KILL, 0, 0, 0);
    if (d <= 0 || st_stop(d, 10) != E_ACCESS_DENIED || st_stop(eh, 10) != E_WRONG_TYPE
        || st_stop(9999, 10) != E_BAD_HANDLE || st_stop(h, SLEEP_MAX_MS + 1) != E_OUT_OF_RANGE
        || proc_stops_armed() != 0) { thread_discard(tid); goto done; }
    htab_close(tab, d); d = -1;

    /* 3: receiving when asked: the notice wakes it, it exits with what the
       receive returned, the deadline is disarmed */
    v = ep_install_handle(tid, ep, HRIGHT_RECV, 0);
    step = 3; if (v <= 0) { thread_discard(tid); goto done; }
    struct ipc_recv_args *ra = (struct ipc_recv_args *)user_ptr(KT_ARGS);
    ra->cap = sizeof *note; ra->buf = ST_NOTE; ra->sender = ST_SENDER; ra->ep_handle = v; ra->reserved = 0;
    *(volatile int32_t *)user_ptr(ST_SENDER) = 0;
    st_call(0, SYS_PROCESS_STOP_ENDPOINT, (uint32_t)v, 0);
    st_call(1, SYS_SLEEP, 1, 0);
    st_call(2, SYS_IPC_RECV, KT_ARGS, 0);
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT || !thread_is_blocked(tid)
        || ep->recv_tid != tid) goto done;
    t0 = timer_ticks();
    if (st_stop(h, 3000) != E_OK || proc_stops_armed() != 1) goto done;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || timer_ticks() - t0 >= 1000
        || pt_info(h) != (int)sizeof *pi || pi->ended != PROC_EXITED
        || pi->code != (int)sizeof *note || pi->killer != 0
        || !st_is_notice((int)sizeof *note, note, user_ptr(ST_SENDER), (uint32_t)thread_current_pid(), 3000)
        || ep->stop_pending || proc_stops_armed() != 0) goto done;

    /* 4: an ended process: 0, and how it ended stands */
    step = 4; if (st_stop(h, 10) != E_OK || proc_stops_armed() != 0 || ep->stop_pending
                  || pt_info(h) <= 0 || pi->ended != PROC_EXITED || pi->code != (int)sizeof *note) goto done;
    htab_close(tab, h); h = -1;

    /* 5: it asks itself before it receives: the notice waits on the
       endpoint, and names the victim as the stopper */
    h = st_make(&tid);
    step = 5; if (h <= 0) goto done;
    int vpid = thread_pid(tid);
    v = ep_install_handle(tid, ep, HRIGHT_RECV, 0);
    int32_t self = htab_install(thread_htab(tid), &thread_proc(tid)->hdr, HRIGHT_STOP, 0);
    if (v <= 0 || self <= 0) { thread_discard(tid); goto done; }
    ra->ep_handle = v;
    st_call(0, SYS_PROCESS_STOP_ENDPOINT, (uint32_t)v, 0);
    st_call(1, SYS_PROCESS_STOP, (uint32_t)self, 2000);
    st_call(2, SYS_IPC_RECV, KT_ARGS, 0);
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || pt_info(h) <= 0
        || pi->ended != PROC_EXITED || pi->code != (int)sizeof *note
        || !st_is_notice((int)sizeof *note, note, user_ptr(ST_SENDER), (uint32_t)vpid, 2000)
        || proc_stops_armed() != 0) goto done;
    htab_close(tab, h); h = -1;

    /* 6: it never receives: still there before the deadline, ended
       PROC_STOPPED after it, and the notice is still on the endpoint */
    h = st_deaf(ep, &tid, &v);
    step = 6; if (h <= 0) goto done;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT || !thread_is_blocked(tid)) goto done;
    t0 = timer_ticks();
    if (st_stop(h, 300) != E_OK || pt_wait(h, SIGNAL_PROCESS_ENDED, 150, 0) != E_TIMED_OUT) goto done;
    if (!st_ends(h, PROC_STOPPED) || timer_ticks() - t0 < 300
        || usermode_faults() != faults0 || !ep->stop_pending || proc_stops_armed() != 0) goto done;
    htab_close(tab, h); h = -1;

    /* 7: a notice that does not fit stays; one that does is from the kernel */
    step = 7; if (st_recv(eh, sizeof(struct stop_notice) - 1) != E_BUFFER_TOO_SMALL || !ep->stop_pending
                  || !st_is_notice(st_recv(eh, sizeof(struct stop_notice)), tbuf, tsend,
                                   (uint32_t)thread_current_pid(), 300)
                  || ep->stop_pending || ep->taken) goto done;

    /* 8: a second stop brings the deadline closer; a third does not put it
       back */
    h = st_deaf(ep, &tid, &v);
    step = 8; if (h <= 0) goto done;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT) goto done;
    /* Under -icount each stop's log lines cost virtual time, hundreds of
       milliseconds. The closer deadline must outlast the third stop's, or it
       has passed before that stop could put it back (a sabotage that let it
       stayed green with 100 ms); and the clock starts after the stops. A
       deadline put back to an hour would not end within st_ends' wait. */
    if (st_stop(h, SLEEP_MAX_MS) != E_OK || proc_stops_armed() != 1
        || st_stop(h, 1500) != E_OK || st_stop(h, SLEEP_MAX_MS) != E_OK
        || proc_stops_armed() != 1) goto done;
    t0 = timer_ticks();
    if (!st_ends(h, PROC_STOPPED) || timer_ticks() - t0 >= 1500) goto done;
    htab_close(tab, h); h = -1;
    if (st_recv(eh, sizeof(struct stop_notice)) != (int)sizeof(struct stop_notice)) goto done;

    /* 9: killed while a stop is pending: the kill's note, and no deadline
       left armed on a process that has gone */
    h = st_deaf(ep, &tid, &v);
    step = 9; if (h <= 0) goto done;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT
        || st_stop(h, SLEEP_MAX_MS) != E_OK || proc_stops_armed() != 1
        || kt_kill_call(h) != E_OK || !st_ends(h, PROC_KILLED)
        || proc_stops_armed() != 0) goto done;
    htab_close(tab, h); h = -1;
    if (st_recv(eh, sizeof(struct stop_notice)) != (int)sizeof(struct stop_notice)) goto done;

    /* 10: nothing registered: ended at once, whatever the deadline */
    h = st_make(&tid);
    step = 10; if (h <= 0) goto done;
    st_call(0, SYS_SLEEP, SLEEP_MAX_MS, 0);
    st_call(1, SYS_SLEEP, SLEEP_MAX_MS, 0);
    st_call(2, SYS_SLEEP, SLEEP_MAX_MS, 0);
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT) goto done;
    t0 = timer_ticks();
    if (st_stop(h, SLEEP_MAX_MS) != E_OK || proc_stops_armed() != 0
        || !st_ends(h, PROC_STOPPED) || timer_ticks() - t0 >= 1000) goto done;
    htab_close(tab, h); h = -1;

    /* 11: registered, but every handle on that endpoint has closed: nobody
       can receive the notice, so ended at once, and the registration's
       reference goes with the process */
    e2 = ep_create();
    ep2 = e2 > 0 ? ep_from_handle(e2, HRIGHT_RECV, 0) : 0;
    step = 11; if (!ep2) goto done;
    h = st_deaf(ep2, &tid, &v);
    if (h <= 0) goto done;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT
        || thread_proc(tid)->stop_ep != ep2) goto done;
    htab_close(thread_htab(tid), v);
    htab_close(tab, e2); e2 = -1;
    if (ep2->active) goto done;
    ep_put(ep2); ep2 = 0;
    t0 = timer_ticks();
    if (st_stop(h, SLEEP_MAX_MS) != E_OK || proc_stops_armed() != 0
        || !st_ends(h, PROC_STOPPED) || timer_ticks() - t0 >= 1000) goto done;
    htab_close(tab, h); h = -1;
    if (ep_in_use_count() != eps0 + 1) goto done;

    /* 12-13: a notice ahead of a staged call: received first, and a reply
       after it answers nothing - not the call behind it, which has not been
       received. Then the call is received as usual. */
    ha = kt_make(&tida);
    step = 12; if (ha <= 0) goto done;
    v = ep_install_handle(tida, ep, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(tida); goto done; }
    struct ipc_call_args *ca = (struct ipc_call_args *)user_ptr(KT_ARGS);
    *(volatile uint8_t *)user_ptr(KT_REQ) = 0x5A;
    ca->req_len = 1; ca->reply_cap = 0; ca->req = KT_REQ; ca->reply = 0;
    ca->ep_handle = v; ca->reserved = 0;
    kt_call(SYS_IPC_CALL, KT_ARGS, 0, 0, 0, 0);
    thread_start(tida);
    if (pt_wait(ha, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT || ep->caller_tid != tida
        || st_reply(eh) != E_INVALID_ARGS || ep->replied || !thread_is_blocked(tida)) goto done;
    if (ep_post_stop(ep, 77, 5) != 0
        || !st_is_notice(st_recv(eh, 64), tbuf, tsend, 77, 5) || ep->taken
        || st_reply(eh) != E_INVALID_ARGS || ep->replied || !thread_is_blocked(tida)) goto done;
    step = 13; if (st_recv(eh, 64) != 1 || *(const uint8_t *)tbuf != 0x5A
                   || *(const int32_t *)tsend != thread_pid(tida) || !ep->taken) goto done;
    if (!kt_kill(ha, tida, 1) || st_reply(eh) != E_PEER_CLOSED) goto done;
    htab_close(tab, ha); ha = -1;

    /* 14: nothing left behind, and nothing ended any other way */
    step = 14; if (proc_live() != live0 || usermode_faults() != faults0
                   || syscall_refused_count() != refused0 || proc_stops_armed() != 0
                   || ep_in_use_count() != eps0 + 1 || mine->stop_ep != 0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "stop test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* A victim still running is killed, so none outlives the test. */
    if (h > 0)  { kt_kill_call(h);  pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0);  htab_close(tab, h); }
    if (ha > 0) { kt_kill_call(ha); pt_wait(ha, SIGNAL_PROCESS_ENDED, 3000, 0); htab_close(tab, ha); }
    if (d > 0)  htab_close(tab, d);
    if (mine) proc_set_stop_endpoint(mine, 0);
    if (ep2) ep_put(ep2);
    if (e2 > 0) htab_close(tab, e2);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- jobs (Phase 1.1d) ----
 * Jobs made through SYS_JOB_CREATE by this thread, whose process is in the
 * kernel's job, with processes in them made the way spawn makes them. Memory
 * charged through vm_charge for a held process and through SYS_VM_MAP and
 * SYS_VM_UNMAP from ring 3; threads counted as processes are made; limits held
 * down a nested job, and changed; a kill of a job reaching a process below
 * it; the refusals; and how long a job lasts. 1 = pass. */
#define JT_LIM  (TEST_CODE_VIRT + 0x800)   /* struct job_limits */
#define JT_INFO (TEST_CODE_VIRT + 0x840)   /* struct job_info */
#define JT_MAP  (TEST_CODE_VIRT + 0x880)   /* the ring-3 victim's first vm_map_args */
#define JT_MAP2 (TEST_CODE_VIRT + 0x8C0)   /* and its second */

static int jt_create(int32_t parent, uint64_t mem, uint32_t threads, uint32_t reserved) {
    struct job_limits *l = (struct job_limits *)user_ptr(JT_LIM);
    l->memory = mem; l->threads = threads; l->reserved = reserved;
    return syscall_dispatch(SYS_JOB_CREATE, (uint32_t)parent, JT_LIM, 0, 0, 0);
}

static int jt_limits(int32_t h, uint64_t mem, uint32_t threads, uint32_t reserved) {
    struct job_limits *l = (struct job_limits *)user_ptr(JT_LIM);
    l->memory = mem; l->threads = threads; l->reserved = reserved;
    return syscall_dispatch(SYS_JOB_SET_LIMITS, (uint32_t)h, JT_LIM, 0, 0, 0);
}

static int jt_kill(int32_t h) {
    return syscall_dispatch(SYS_JOB_KILL, (uint32_t)h, 0, 0, 0, 0);
}

static int32_t jt_dup(int32_t h, uint32_t rights) {
    return syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)h, rights, 0, 0, 0);
}

/* OBJINFO_JOB (or `topic`) on `h` into JT_INFO, with room for `cap`. */
static int jt_info_as(int32_t h, uint32_t topic, uint64_t cap) {
    struct object_info_args *oi = (struct object_info_args *)user_ptr(PT_IARGS);
    oi->cap = cap; oi->buf = JT_INFO; oi->handle = h; oi->topic = topic;
    return syscall_dispatch(SYS_OBJECT_INFO, PT_IARGS, 0, 0, 0, 0);
}

static int jt_info(int32_t h) {
    return jt_info_as(h, OBJINFO_JOB, sizeof(struct job_info));
}

/* `h`'s job holds `used` bytes, `threads` threads and `procs` processes. */
static int jt_is(int32_t h, uint64_t used, uint32_t threads, uint32_t procs) {
    const struct job_info *ji = (const struct job_info *)user_ptr(JT_INFO);
    return jt_info(h) == (int)sizeof *ji && ji->memory_used == used
        && ji->threads == threads && ji->processes == procs;
}

/* The job `h` names; the handle's reference keeps it. */
static struct job *jt_job(int32_t h) {
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_JOB, 0, 0);
    if (o) kobj_unref(o);
    return (struct job *)o;
}

/* A held process in `j` with a memory account, which never runs. */
static int32_t jt_held(struct job *j, int *tid) {
    int32_t h = pt_make_in(user_blob_exit_start, user_blob_exit_end, tid, j);
    if (h > 0) vm_proc_init(thread_pid(*tid), vm_default_quota(), -1);
    return h;
}

/* A held process in `j` that sleeps for the longest sleep there is. */
static int32_t jt_sleeper(struct job *j, int *tid) {
    kt_len = (uint32_t)(user_blob_call_end - user_blob_call_start);
    if (kt_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t i = 0; i < kt_len; i++) kt_image[i] = user_blob_call_start[i];
    kt_call(SYS_SLEEP, SLEEP_MAX_MS, 0, 0, 0, 0);
    return pt_make_in(kt_image, kt_image + kt_len + 6 * 8, tid, j);
}

/* A held process in `j` that maps a page, unmaps it, then asks for two and
   exits with what that returned. */
static int32_t jt_mapper(struct job *j, int *tid) {
    struct vm_map_args *m = (struct vm_map_args *)user_ptr(JT_MAP);
    struct vm_map_args *m2 = (struct vm_map_args *)user_ptr(JT_MAP2);
    m->offset = 0; m->length = PAGE_SIZE; m->granted = 0; m->addr = 0;
    m->object = MOBJ_ANON; m->prot = MPROT_READ | MPROT_WRITE; m->flags = 0; m->reserved = 0;
    *m2 = *m;
    m2->length = 2u * PAGE_SIZE;
    st_len = (uint32_t)(user_blob_calls_end - user_blob_calls_start);
    if (st_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t i = 0; i < st_len; i++) st_image[i] = user_blob_calls_start[i];
    st_call(0, SYS_VM_MAP, JT_MAP, 0);
    st_call(1, SYS_VM_UNMAP, VM_MMAP_BASE, PAGE_SIZE);
    st_call(2, SYS_VM_MAP, JT_MAP2, 0);
    int32_t h = pt_make_in(st_image, st_image + st_len + 3 * 6 * 8, tid, j);
    if (h > 0) vm_proc_init(thread_pid(*tid), vm_default_quota(), -1);
    return h;
}

int usermode_job_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    struct job_info *ji = (struct job_info *)user_ptr(JT_INFO);
    struct object_basic *ob = (struct object_basic *)user_ptr(JT_INFO);
    const uint32_t P = PAGE_SIZE;
    const uint32_t all = HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_KILL | HRIGHT_ASSIGN
                       | HRIGHT_SET_LIMITS | HRIGHT_DUPLICATE;
    uint32_t live0 = proc_live(), jobs0 = job_live();
    uint32_t faults0 = usermode_faults(), refused0 = syscall_refused_count();
    int32_t j = -1, c = -1, k = -1, e = -1, d = -1, h = -1, h2 = -1;
    int32_t chain[JOB_DEPTH_MAX];
    int tid = -1, tid2 = -1, step = 0, ok = 0, vpid;
    for (uint32_t n = 0; n < JOB_DEPTH_MAX; n++) chain[n] = -1;

    /* 1: making one: the refusals, then one with limits, its rights, its
       account; the rights each call needs */
    step = 1; if (jt_create(9999, 0, 0, 0) != E_BAD_HANDLE || jt_create(0, 0, 0, 1) != E_INVALID_ARGS
                  || syscall_dispatch(SYS_JOB_CREATE, 0, 8, 0, 0, 0) != E_BAD_ADDRESS
                  || job_live() != jobs0) goto done;
    j = jt_create(0, 2u * P, 2, 0);
    if (j <= 0 || job_live() != jobs0 + 1) goto done;
    if (jt_info_as(j, OBJINFO_BASIC, sizeof *ob) != (int)sizeof *ob
        || ob->kind != OBJ_TYPE_JOB || ob->rights != all) goto done;
    if (jt_info(j) != (int)sizeof *ji || ji->memory_limit != 2u * P || ji->thread_limit != 2
        || ji->memory_used || ji->memory_peak || ji->threads || ji->processes || ji->killed
        || jt_info_as(j, OBJINFO_JOB, sizeof *ji - 1) != E_BUFFER_TOO_SMALL) goto done;
    d = jt_dup(j, HRIGHT_KILL);
    if (d <= 0 || jt_info(d) != E_ACCESS_DENIED || jt_create(d, 0, 0, 0) != E_ACCESS_DENIED
        || jt_limits(d, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    htab_close(tab, d);
    d = jt_dup(j, HRIGHT_INSPECT);
    if (d <= 0 || jt_kill(d) != E_ACCESS_DENIED || jt_info(j) <= 0 || ji->killed) goto done;
    htab_close(tab, d); d = -1;

    /* 2: nested JOB_DEPTH_MAX deep below the kernel's and no deeper; each
       holds its parent, so a handle closed in the middle frees nothing */
    int32_t at = j;
    for (uint32_t n = 0; n + 1 < JOB_DEPTH_MAX; n++) {
        chain[n] = jt_create(at, 0, 0, 0);
        step = 2; if (chain[n] <= 0) goto done;
        at = chain[n];
    }
    if (jt_create(at, 0, 0, 0) != E_OUT_OF_RANGE || job_live() != jobs0 + JOB_DEPTH_MAX) goto done;
    htab_close(tab, chain[0]); chain[0] = -1;
    if (job_live() != jobs0 + JOB_DEPTH_MAX) goto done;
    for (uint32_t n = 1; n + 1 < JOB_DEPTH_MAX; n++) { htab_close(tab, chain[n]); chain[n] = -1; }
    if (job_live() != jobs0 + 1) goto done;

    /* 3: memory through vm_charge, for a process in it: to the limit and not
       past it, a refusal leaving both accounts as they were; an uncharge; a
       limit lowered below the use (nothing taken back, the next charge
       refused); the process's end giving back what it held */
    h = jt_held(jt_job(j), &tid);
    step = 3; if (h <= 0 || !jt_is(j, 0, 1, 1)) goto done;
    vpid = thread_pid(tid);
    struct vm_proc *vm = &thread_proc(tid)->vm;
    if (vm_charge(vpid, P) != E_OK || !jt_is(j, P, 1, 1)) goto done;
    if (vm_charge(vpid, 2u * P) != E_NO_MEMORY || vm->mapped != P || !jt_is(j, P, 1, 1)) goto done;
    if (vm_charge(vpid, P) != E_OK || !jt_is(j, 2u * P, 1, 1) || ji->memory_peak != 2u * P) goto done;
    vm_uncharge(vpid, P);
    if (!jt_is(j, P, 1, 1) || ji->memory_peak != 2u * P) goto done;
    if (jt_limits(j, 1, 2, 0) != E_OK || !jt_is(j, P, 1, 1) || ji->memory_limit != 1
        || vm_charge(vpid, P) != E_NO_MEMORY || vm->mapped != P) goto done;
    if (jt_limits(j, 2u * P, 2, 1) != E_INVALID_ARGS || jt_limits(h, 0, 0, 0) != E_WRONG_TYPE
        || jt_info(h) != E_WRONG_TYPE || jt_kill(h) != E_WRONG_TYPE
        || jt_limits(j, 2u * P, 2, 0) != E_OK) goto done;
    thread_discard(tid); tid = -1;
    if (!jt_is(j, 0, 0, 0)) goto done;
    htab_close(tab, h); h = -1;

    /* 4: a job below it with no limits of its own is held to j's, memory and
       threads both, and both accounts count what is in it */
    c = jt_create(j, 0, 0, 0);
    h = c > 0 ? jt_held(jt_job(c), &tid) : -1;
    step = 4; if (h <= 0) goto done;
    vpid = thread_pid(tid);
    if (vm_charge(vpid, 3u * P) != E_NO_MEMORY || vm_charge(vpid, 2u * P) != E_OK
        || !jt_is(c, 2u * P, 1, 1) || !jt_is(j, 2u * P, 1, 1)) goto done;
    if (jt_limits(j, 2u * P, 1, 0) != E_OK) goto done;
    h2 = jt_held(jt_job(c), &tid2);
    if (h2 != E_NO_RESOURCES || !jt_is(c, 2u * P, 1, 1) || !jt_is(j, 2u * P, 1, 1)) goto done;
    tid2 = -1;
    if (jt_limits(j, 2u * P, 2, 0) != E_OK) goto done;
    h2 = jt_held(jt_job(c), &tid2);
    if (h2 <= 0 || !jt_is(c, 2u * P, 2, 2) || !jt_is(j, 2u * P, 2, 2)) goto done;
    thread_discard(tid);  tid = -1;
    thread_discard(tid2); tid2 = -1;
    if (!jt_is(c, 0, 0, 0) || !jt_is(j, 0, 0, 0)) goto done;
    htab_close(tab, h);  h = -1;
    htab_close(tab, h2); h2 = -1;

    /* 5: from ring 3, in a job of one page: a page mapped, unmapped, then two
       refused; nothing left mapped, and the job's peak was that one page */
    k = jt_create(0, P, 0, 0);
    h = k > 0 ? jt_mapper(jt_job(k), &tid) : -1;
    step = 5; if (h <= 0) goto done;
    thread_start(tid); tid = -1;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK || pt_info(h) != (int)sizeof *pi
        || pi->ended != PROC_EXITED || pi->code != E_NO_MEMORY) goto done;
    if (jt_info(k) != (int)sizeof *ji || ji->memory_used != 0 || ji->memory_peak != P
        || ji->processes != 0 || paging_get_phys(VM_MMAP_BASE) != 0) goto done;
    htab_close(tab, h); h = -1;

    /* 6: a kill of j reaches a process in it and one in c, each ended killed
       by this thread's process; j and c are signalled ENDED and closed to new
       jobs and processes; a second kill changes nothing */
    h  = jt_sleeper(jt_job(j), &tid);
    h2 = jt_sleeper(jt_job(c), &tid2);
    step = 6; if (h <= 0 || h2 <= 0) goto done;
    thread_start(tid);  tid = -1;
    thread_start(tid2); tid2 = -1;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT
        || pt_wait(j, SIGNAL_JOB_ENDED, 0, 0) != E_TIMED_OUT || !jt_is(j, 0, 2, 2)
        || jt_kill(9999) != E_BAD_HANDLE) goto done;
    if (jt_kill(j) != E_OK || !st_ends(h, PROC_KILLED) || !st_ends(h2, PROC_KILLED)) goto done;
    if (pt_wait(j, SIGNAL_JOB_ENDED, 3000, 0) != E_OK || pt_wait(c, SIGNAL_JOB_ENDED, 3000, 0) != E_OK
        || jt_info(j) != (int)sizeof *ji || ji->killed != 1 || ji->processes != 0
        || jt_info(c) != (int)sizeof *ji || ji->killed != 1) goto done;
    if (jt_create(j, 0, 0, 0) != E_CANCELED || jt_create(c, 0, 0, 0) != E_CANCELED
        || jt_held(jt_job(c), &tid) != E_CANCELED) goto done;
    tid = -1;
    if (jt_kill(j) != E_OK || pt_info(h) <= 0 || pi->ended != PROC_KILLED) goto done;
    htab_close(tab, h);  h = -1;
    htab_close(tab, h2); h2 = -1;

    /* 7: an empty job, killed, is ended at once */
    step = 7; if (jt_kill(k) != E_OK || pt_wait(k, SIGNAL_JOB_ENDED, 0, 0) != E_OK) goto done;

    /* 8: a job outlives its last handle while a process is in it, and goes
       with that process's record */
    e = jt_create(0, 0, 0, 0);
    h = e > 0 ? jt_held(jt_job(e), &tid) : -1;
    step = 8; if (h <= 0) goto done;
    uint32_t jobs1 = job_live();
    htab_close(tab, e); e = -1;
    if (job_live() != jobs1) goto done;
    thread_discard(tid); tid = -1;
    if (job_live() != jobs1) goto done;
    htab_close(tab, h); h = -1;
    if (job_live() != jobs1 - 1) goto done;

    /* 9: nothing left behind, and nothing ended any other way */
    htab_close(tab, k); k = -1;
    htab_close(tab, c); c = -1;
    htab_close(tab, j); j = -1;
    step = 9; if (job_live() != jobs0 || proc_live() != live0 || usermode_faults() != faults0
                  || syscall_refused_count() != refused0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "job test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* Held ones are discarded; one still running is killed, so none outlives
       the test. */
    if (tid >= 0)  thread_discard(tid);
    if (tid2 >= 0) thread_discard(tid2);
    if (h > 0)  { kt_kill_call(h);  pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0);  htab_close(tab, h); }
    if (h2 > 0) { kt_kill_call(h2); pt_wait(h2, SIGNAL_PROCESS_ENDED, 3000, 0); htab_close(tab, h2); }
    for (uint32_t n = 0; n < JOB_DEPTH_MAX; n++) if (chain[n] > 0) htab_close(tab, chain[n]);
    if (d > 0) htab_close(tab, d);
    if (e > 0) htab_close(tab, e);
    if (k > 0) htab_close(tab, k);
    if (c > 0) htab_close(tab, c);
    if (j > 0) htab_close(tab, j);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- the monotonic clock and timer objects (Phase 1.2a) --------------------
 * Through syscall_dispatch over a user mapping, as a program makes the calls.
 * Nothing logs between taking a time and checking it: under -icount a log
 * line costs hundreds of virtual ms. 1 = pass, 0 = fail. */
#define TT_NOW   (TEST_CODE_VIRT + 0x100)   /* SYS_CLOCK_MONOTONIC's *u64 */
#define TT_INFO  (TEST_CODE_VIRT + 0x200)   /* struct object_basic */
#define TT_IARGS (TEST_CODE_VIRT + 0x300)   /* struct object_info_args */
#define TT_SEEN  (TEST_CODE_VIRT + 0x400)
#define TT_MS(n) ((uint64_t)(n) * TIMER_NS_PER_TICK)

static int tt_wait(int32_t h, uint64_t deadline) {
    return syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)h, SIGNAL_TIMER_FIRED, deadline, 0, 0);
}
static int tt_look(int32_t h) { return tt_wait(h, 0); }
static int tt_set(int32_t h, uint64_t deadline) {
    return syscall_dispatch(SYS_TIMER_SET, (uint32_t)h, deadline, 0, 0, 0);
}
static int tt_cancel(int32_t h) { return syscall_dispatch(SYS_TIMER_CANCEL, (uint32_t)h, 0, 0, 0, 0); }

int usermode_timer_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    volatile uint64_t *now = (volatile uint64_t *)user_ptr(TT_NOW);
    volatile uint32_t *seen = (volatile uint32_t *)user_ptr(TT_SEEN);
    uint32_t live0 = kobj_live(KOBJ_TIMER), armed0 = ktimer_armed_count();
    int32_t t = -1, u = -1, d = -1, eh = -1;
    int step = 0, ok = 0;

    /* 1-2: the clock: whole ticks in ns, never back, and it measures a sleep */
    *now = 1;
    step = 1; if (syscall_dispatch(SYS_CLOCK_MONOTONIC, TT_NOW, 0, 0, 0, 0) != E_OK
                  || *now == 1 || *now % TIMER_NS_PER_TICK != 0
                  || *now > timer_now_ns()
                  || syscall_dispatch(SYS_CLOCK_MONOTONIC, USER_VLIMIT, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    uint64_t c0 = *now;
    syscall_dispatch(SYS_SLEEP, 20, 0, 0, 0, 0);
    syscall_dispatch(SYS_CLOCK_MONOTONIC, TT_NOW, 0, 0, 0, 0);
    step = 2; if (*now < c0 + TT_MS(20)) goto done;

    /* 3-4: a new timer, with the rights it is made with; disarmed, unfired */
    t = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    struct object_info_args *oi = (struct object_info_args *)user_ptr(TT_IARGS);
    struct object_basic *ob = (struct object_basic *)user_ptr(TT_INFO);
    oi->cap = sizeof *ob; oi->buf = TT_INFO; oi->handle = t; oi->topic = OBJINFO_BASIC;
    step = 3; if (t <= 0 || syscall_dispatch(SYS_OBJECT_INFO, TT_IARGS, 0, 0, 0, 0) != (int)sizeof *ob
                  || ob->kind != OBJ_TYPE_TIMER
                  || ob->rights != (HRIGHT_WAIT | HRIGHT_ARM | HRIGHT_DUPLICATE)
                  || kobj_live(KOBJ_TIMER) != live0 + 1) goto done;
    *seen = 0xFFFFFFFFu;
    step = 4; if (syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)t, SIGNAL_TIMER_FIRED, 0, TT_SEEN, 0) != E_TIMED_OUT
                  || *seen != 0 || ktimer_armed_count() != armed0) goto done;

    /* 5: a deadline already passed fires it at once, and arms nothing */
    step = 5; if (tt_set(t, timer_now_ns()) != E_OK || tt_look(t) != E_OK
                  || ktimer_armed_count() != armed0) goto done;

    /* 6: arming again clears the signal; it fires at the deadline, not
       before, and leaves the list */
    uint64_t dl = timer_now_ns() + TT_MS(30);
    step = 6; if (tt_set(t, dl) != E_OK || tt_look(t) != E_TIMED_OUT
                  || ktimer_armed_count() != armed0 + 1
                  || tt_wait(t, DEADLINE_NEVER) != E_OK || timer_now_ns() < dl
                  || timer_now_ns() > dl + TT_MS(200)
                  || ktimer_armed_count() != armed0) goto done;

    /* 7: a new deadline replaces the old: armed for 20 ms, then for 60, it
       has not fired at 40 */
    uint64_t base = timer_now_ns();
    tt_set(t, base + TT_MS(20));
    step = 7; if (tt_set(t, base + TT_MS(60)) != E_OK || ktimer_armed_count() != armed0 + 1
                  || tt_wait(t, base + TT_MS(40)) != E_TIMED_OUT
                  || tt_wait(t, DEADLINE_NEVER) != E_OK || timer_now_ns() < base + TT_MS(60)) goto done;

    /* 8-9: cancel disarms (nothing at its deadline) and clears a fired one */
    base = timer_now_ns();
    step = 8; if (tt_set(t, base + TT_MS(20)) != E_OK || tt_cancel(t) != E_OK
                  || ktimer_armed_count() != armed0
                  || tt_wait(t, base + TT_MS(50)) != E_TIMED_OUT) goto done;
    step = 9; if (tt_set(t, 0) != E_OK || tt_look(t) != E_OK
                  || tt_cancel(t) != E_OK || tt_look(t) != E_TIMED_OUT) goto done;

    /* 10: two timers fire in deadline order, not the order armed */
    u = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    base = timer_now_ns();
    step = 10; if (u <= 0 || tt_set(t, base + TT_MS(40)) != E_OK || tt_set(u, base + TT_MS(20)) != E_OK
                   || ktimer_armed_count() != armed0 + 2
                   || tt_wait(u, DEADLINE_NEVER) != E_OK || tt_look(t) != E_TIMED_OUT
                   || tt_wait(t, DEADLINE_NEVER) != E_OK || timer_now_ns() < base + TT_MS(40)) goto done;

    /* 11-14: refusals, each its own error */
    step = 11; if (tt_set(t, DEADLINE_NEVER) != E_INVALID_ARGS
                   || tt_set(t, DEADLINE_NEVER + 1) != E_INVALID_ARGS
                   || syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)t, SIGNAL_TIMER_FIRED << 1, 0, 0, 0) != E_INVALID_ARGS
                   || ktimer_armed_count() != armed0) goto done;
    step = 12; if (tt_set(9999, 0) != E_BAD_HANDLE || tt_cancel(9999) != E_BAD_HANDLE) goto done;
    eh = ep_create();
    step = 13; if (eh <= 0 || tt_set(eh, 0) != E_WRONG_TYPE || tt_cancel(eh) != E_WRONG_TYPE) goto done;
    /* each right is needed: no ARM, no set or cancel; no WAIT, no wait */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)t, HRIGHT_WAIT, 0, 0, 0);
    step = 14; if (d <= 0 || tt_set(d, 0) != E_ACCESS_DENIED || tt_cancel(d) != E_ACCESS_DENIED
                   || tt_look(t) != E_OK) goto done;   /* fired at step 10, and set refused */
    htab_close(tab, d);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)t, HRIGHT_ARM | HRIGHT_DUPLICATE, 0, 0, 0);
    step = 15; if (d <= 0 || tt_look(d) != E_ACCESS_DENIED || tt_set(d, 0) != E_OK) goto done;

    /* 16-17: a timer stays armed while any handle is open, and is disarmed and
       freed with the last */
    step = 16; if (tt_set(t, timer_now_ns() + TT_MS(10000)) != E_OK
                   || ktimer_armed_count() != armed0 + 1) goto done;
    htab_close(tab, t); t = -1;
    step = 16; if (ktimer_armed_count() != armed0 + 1) goto done;   /* d still holds it */
    htab_close(tab, d); d = -1;
    htab_close(tab, u); u = -1;
    step = 17; if (ktimer_armed_count() != armed0 || kobj_live(KOBJ_TIMER) != live0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "timer test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (t > 0)  htab_close(tab, t);
    if (u > 0)  htab_close(tab, u);
    if (d > 0)  htab_close(tab, d);
    if (eh > 0) htab_close(tab, eh);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}
