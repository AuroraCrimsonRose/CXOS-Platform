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
#include "object/knotify.h"   /* SYS_NOTIFY_* */
#include "object/kmem.h"      /* SYS_MEM_CREATE */
#include "object/kpipe.h"     /* SYS_PIPE_CREATE, pipe reads and writes */
#include "object/kqueue.h"    /* SYS_QUEUE_* */
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

/* Unprivileged within limits: a thread may always go lower, and up to its
   job's ceiling; real-time is GRANT_REALTIME's, since a thread there that
   never blocks starves everything below it. */
int syscall_thread_set_priority(uint32_t level) {
    int me = thread_current_id();
    if (level < PRIO_MIN || level > PRIO_MAX) return E_OUT_OF_RANGE;
    if (level > thread_base(me)) {
        struct proc *p = thread_proc(me);
        if (level >= PRIO_REALTIME && !(thread_current_caps() & GRANT_REALTIME)) return E_ACCESS_DENIED;
        if (!p || level > job_prio_ceiling(p->job)) return E_ACCESS_DENIED;
    }
    ipc_set_base(me, level);
    return E_OK;
}

int syscall_thread_priority(void) {
    int me = thread_current_id();
    return (int)(thread_base(me) | thread_prio(me) << 8);
}

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

/* READ and WRITE on a pipe end: the handle's right is the authority, so the
   disk gate is not asked. 1 with *rc set when `h` is a pipe end; 0 (nothing
   done) for anything else, which the file path then judges. */
static int pipe_io(int32_t h, uint64_t buf, uint64_t n, int write, int *rc) {
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_PIPE, 0, &hi);
    if (!o) return 0;
    struct kpipe_end *e = (struct kpipe_end *)o;
    if (n > 0x7FFFFFFFu) n = 0x7FFFFFFFu;   /* the count comes back in an i32: a short one is normal */
    *rc = !(hi.rights & (write ? HRIGHT_WRITE : HRIGHT_READ))   ? E_ACCESS_DENIED
        : n == 0                                                ? 0
        : write ? (!user_ptr_readable(buf, n) ? E_BAD_ADDRESS : kpipe_write(e, buf, (uint32_t)n))
        :         (!user_ptr_writable(buf, n) ? E_BAD_ADDRESS : kpipe_read(e, buf, (uint32_t)n));
    kobj_unref(o);   /* a blocked read or write held the end until here */
    return 1;
}

int syscall_file_read(int32_t h, uint64_t buf, uint64_t n) {
    int rc;
    if (pipe_io(h, buf, n, 0, &rc)) return rc;
    if (!disk_granted()) return E_ACCESS_DENIED;
    return sys_file_read(h, buf, n);
}

int syscall_file_write(int32_t h, uint64_t buf, uint64_t n) {
    int rc;
    if (pipe_io(h, buf, n, 1, &rc)) return rc;
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
    struct vm_map_args *va = (struct vm_map_args *)user_ptr(a);
    if (va->object == MOBJ_ANON) return vm_map(thread_current_pid(), va);
    /* A memory object: the handle's rights decide what the view may do. */
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), (int32_t)va->object,
                              KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_MEM ? E_WRONG_TYPE
           : vm_map_object(thread_current_pid(), va, (struct kmem *)o, hi.rights);
    kobj_unref(o);   /* a mapping holds its own reference */
    return rc;
}

int syscall_pipe_create(uint32_t size, uint64_t ends) {
    /* Unprivileged: the ring is charged to the caller's job, which is the
       bound, and its handles are all the authority there is over it. */
    if (size == 0) size = PIPE_SIZE_DEFAULT;
    if (size < PIPE_SIZE_MIN || size > PIPE_SIZE_MAX) return E_OUT_OF_RANGE;
    if (!user_ptr_writable(ends, 2 * sizeof(int32_t))) return E_BAD_ADDRESS;
    struct htab *tab = thread_htab(thread_current_id());
    int rc;
    struct kpipe *p = kpipe_create(size, thread_proc(thread_current_id())->job, &rc);
    if (!p) return rc;
    int32_t h[2];
    h[0] = htab_install(tab, &p->rd.hdr, HRIGHT_READ | HRIGHT_WAIT | HRIGHT_DUPLICATE, 0);
    h[1] = h[0] < 0 ? h[0] : htab_install(tab, &p->wr.hdr, HRIGHT_WRITE | HRIGHT_WAIT | HRIGHT_DUPLICATE, 0);
    rc = h[1] < 0 ? h[1] : user_copy_out(ends, h, sizeof h) != 0 ? E_BAD_ADDRESS : E_OK;
    /* Both handles or neither: a caller that never learnt them could not
       close them. Each end goes with its last reference. */
    if (rc != E_OK) {
        if (h[0] > 0) htab_close(tab, h[0]);
        if (h[1] > 0) htab_close(tab, h[1]);
    }
    kobj_unref(&p->rd.hdr);
    kobj_unref(&p->wr.hdr);
    return rc;
}

int syscall_mem_create(uint64_t size, uint32_t flags) {
    /* Unprivileged, as SYS_VM_MAP is: what bounds it is the commit, charged
       to the caller's job, and the memory there is to promise. */
    if (flags)                 return E_INVALID_ARGS;
    if (size == 0)             return E_INVALID_ARGS;
    if (size > MMAP_MAX_BYTES) return E_OUT_OF_RANGE;
    int rc;
    struct kmem *m = kmem_create((uint32_t)((size + PAGE_SIZE - 1u) / PAGE_SIZE),
                                 thread_proc(thread_current_id())->job, &rc);
    if (!m) return rc;
    int32_t h = htab_install(thread_htab(thread_current_id()), &m->hdr,
                             HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_MAP_EXEC |
                             HRIGHT_SEAL | HRIGHT_INSPECT | HRIGHT_DUPLICATE, 0);
    kobj_unref(&m->hdr);   /* the handle's reference keeps it; with no handle, it goes */
    return h;
}

int syscall_mem_seal(int32_t h, uint32_t seals) {
    /* Unprivileged: the right is the authority, and a seal only takes away. */
    if (!seals || (seals & ~(MEM_SEAL_WRITE | MEM_SEAL_SEAL))) return E_INVALID_ARGS;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_MEM         ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_SEAL)  ? E_ACCESS_DENIED
           : kmem_seal((struct kmem *)o, seals);
    kobj_unref(o);
    return rc;
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

/* Unprivileged: it needs the endpoint's receive handle, which only its
   creator holds unless it gave one away. */
int syscall_ep_mint(int32_t h, uint64_t badge, uint32_t rights) { return ep_mint(h, badge, rights); }

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
            ji.priority_limit = j->prio_limit;
            arch_irq_restore(f);
        }
        kobj_unref(o);
        if (rc != E_OK) return rc;
        if (user_copy_out(oa.buf, &ji, sizeof ji) != 0) return E_BAD_ADDRESS;
        return (int)sizeof ji;
    }
    if (oa.topic == OBJINFO_MEM) {
        struct kmem *m = (struct kmem *)o;
        int rc = o->type != KOBJ_MEM              ? E_WRONG_TYPE
               : !(hi.rights & HRIGHT_INSPECT)    ? E_ACCESS_DENIED
               : oa.cap < sizeof(struct mem_info) ? E_BUFFER_TOO_SMALL : E_OK;
        struct mem_info mi = { 0 };
        if (rc == E_OK) {
            mi.size     = (uint64_t)m->npages * PAGE_SIZE;
            mi.resident = (uint64_t)kmem_resident(m) * PAGE_SIZE;
            mi.seals    = kmem_seals(m);
        }
        kobj_unref(o);
        if (rc != E_OK) return rc;
        if (user_copy_out(oa.buf, &mi, sizeof mi) != 0) return E_BAD_ADDRESS;
        return (int)sizeof mi;
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

int syscall_timer_create(uint64_t slack) {
    /* Unprivileged: a timer is a handle, held to the handle table's limit.
       Any slack: a deadline and its slack past DEADLINE_NEVER are held to it
       (ktimer.c), and a late timer delays only its own waiters. */
    struct ktimer *t = ktimer_create(slack);
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

int syscall_notify_create(void) {
    /* Unprivileged, as a timer is: a handle, held to the table's limit. */
    struct knotify *n = knotify_create();
    if (!n) return E_NO_MEMORY;
    int32_t h = htab_install(thread_htab(thread_current_id()), &n->hdr,
                             HRIGHT_WAIT | HRIGHT_SIGNAL | HRIGHT_RECV | HRIGHT_DUPLICATE, 0);
    kobj_unref(&n->hdr);   /* the handle's reference keeps it; with no handle, it goes */
    return h;
}

/* The notification behind `h`, with a reference, if the handle holds `need`. */
static struct knotify *notify_from_handle(int32_t h, uint32_t need, struct htab_info *hi, int *rc) {
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, hi);
    if (!o) { *rc = E_BAD_HANDLE; return 0; }
    *rc = o->type != KOBJ_NOTIFY     ? E_WRONG_TYPE
        : (hi->rights & need) != need ? E_ACCESS_DENIED : E_OK;
    if (*rc != E_OK) { kobj_unref(o); return 0; }
    return (struct knotify *)o;
}

int syscall_notify_mint(int32_t h, uint64_t bits, uint32_t rights) {
    struct htab_info hi;
    int rc;
    struct knotify *n = notify_from_handle(h, HRIGHT_RECV, &hi, &rc);
    if (!n) return rc;
    if (!bits) rc = E_INVALID_ARGS;
    else if (!(rights & HRIGHT_SIGNAL) || (rights & ~(HRIGHT_SIGNAL | HRIGHT_DUPLICATE)))
        rc = E_ACCESS_DENIED;
    if (rc == E_OK) {
        struct ksignaller *s = ksignaller_create(n, bits);
        if (!s) rc = E_NO_MEMORY;
        else {
            /* Derived from the notification's grant, as a minted send handle
               is from its endpoint's. */
            rc = htab_install(thread_htab(thread_current_id()), &s->hdr, rights, hi.grant);
            kobj_unref(&s->hdr);
        }
    }
    kobj_unref(&n->hdr);
    return rc;
}

int syscall_notify_signal(int32_t h, uint64_t bits) {
    /* Unprivileged: HRIGHT_SIGNAL is the authority, and a signaller's bits
       are the limit of it. */
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    struct knotify *n = o->type == KOBJ_NOTIFY    ? (struct knotify *)o
                      : o->type == KOBJ_SIGNALLER ? ((struct ksignaller *)o)->n : 0;
    int rc = !n                                ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_SIGNAL)      ? E_ACCESS_DENIED
           : !bits                             ? E_INVALID_ARGS
           : o->type == KOBJ_SIGNALLER && (bits & ~((struct ksignaller *)o)->bits)
                                               ? E_ACCESS_DENIED
           : knotify_signal(n, bits);
    kobj_unref(o);   /* a signaller's reference keeps `n` until here */
    return rc;
}

int syscall_notify_take(int32_t h, uint64_t bits) {
    struct htab_info hi;
    int rc;
    struct knotify *n = notify_from_handle(h, HRIGHT_RECV, &hi, &rc);
    if (!n) return rc;
    uint64_t w = knotify_take(n);
    if (user_copy_out(bits, &w, sizeof w) != 0) {
        /* Nothing taken: the bits go back, with whatever came since. If
           another thread closed the last handle meanwhile, nobody could take
           them anyway. */
        if (w) knotify_signal(n, w);
        rc = E_BAD_ADDRESS;
    }
    kobj_unref(&n->hdr);
    return rc;
}

int syscall_queue_create(void) {
    /* Unprivileged: a queue is a handle, held to the handle table's limit,
       and its registrations to QUEUE_REGISTER_MAX. */
    struct kqueue *q = kqueue_create();
    if (!q) return E_NO_MEMORY;
    int32_t h = htab_install(thread_htab(thread_current_id()), &q->hdr,
                             HRIGHT_WAIT | HRIGHT_REGISTER | HRIGHT_DUPLICATE, 0);
    kobj_unref(&q->hdr);   /* the handle's reference keeps it; with no handle, it goes */
    return h;
}

/* The queue behind `h`, with a reference, if the handle holds `right`. */
static struct kqueue *queue_from_handle(int32_t h, uint32_t right, int *rc) {
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) { *rc = E_BAD_HANDLE; return 0; }
    *rc = o->type != KOBJ_QUEUE  ? E_WRONG_TYPE
        : !(hi.rights & right)   ? E_ACCESS_DENIED : E_OK;
    if (*rc != E_OK) { kobj_unref(o); return 0; }
    return (struct kqueue *)o;
}

int syscall_queue_register(int32_t qh, int32_t h, uint32_t signals, uint64_t key) {
    /* Unprivileged: the queue's HRIGHT_REGISTER and the object's HRIGHT_WAIT
       are the authority. The object is checked as SYS_OBJECT_WAIT checks it. */
    int rc;
    struct kqueue *q = queue_from_handle(qh, HRIGHT_REGISTER, &rc);
    if (!q) return rc;
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) { kobj_unref(&q->hdr); return E_BAD_HANDLE; }
    const struct kobj_ops *ops = kobj_type_ops((enum kobj_type)o->type);
    rc = !(hi.rights & HRIGHT_WAIT)                       ? E_ACCESS_DENIED
       : !ops->signals_valid                               ? E_NOT_SUPPORTED
       : !signals || (signals & ~ops->signals_valid)       ? E_INVALID_ARGS
       : kqueue_register(q, o, signals, key);
    kobj_unref(o);
    kobj_unref(&q->hdr);
    return rc;
}

int syscall_queue_arm(int32_t qh, uint64_t key) {
    int rc;
    struct kqueue *q = queue_from_handle(qh, HRIGHT_REGISTER, &rc);
    if (!q) return rc;
    rc = kqueue_arm(q, key);
    kobj_unref(&q->hdr);
    return rc;
}

int syscall_queue_unregister(int32_t qh, uint64_t key) {
    int rc;
    struct kqueue *q = queue_from_handle(qh, HRIGHT_REGISTER, &rc);
    if (!q) return rc;
    rc = kqueue_unregister(q, key);
    kobj_unref(&q->hdr);
    return rc;
}

int syscall_queue_wait(int32_t qh, uint64_t events, uint32_t count, uint64_t deadline) {
    /* Unprivileged: the queue's HRIGHT_WAIT is the authority. A count past
       QUEUE_EVENTS_MAX is the size of the caller's array, not an error. */
    if (!count) return E_INVALID_ARGS;
    uint32_t n = count > QUEUE_EVENTS_MAX ? QUEUE_EVENTS_MAX : count;
    if (!user_ptr_writable(events, (uint64_t)n * sizeof(struct queue_event))) return E_BAD_ADDRESS;
    int rc;
    struct kqueue *q = queue_from_handle(qh, HRIGHT_WAIT, &rc);
    if (!q) return rc;
    struct queue_event out[QUEUE_EVENTS_MAX];
    rc = kqueue_wait(q, out, n, deadline);
    kobj_unref(&q->hdr);
    /* Written after the wait, through the copy that checks the range again:
       the page may have been unmapped while this thread slept. The keys
       taken are idle either way. */
    if (rc > 0 && user_copy_out(events, out, (uint32_t)rc * (uint32_t)sizeof out[0]) != 0) return E_BAD_ADDRESS;
    return rc;
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
    l->memory = 0; l->threads = 0; l->priority = 0;
    if (!u) return E_OK;
    if (user_copy_in(l, u, sizeof *l) != 0) return E_BAD_ADDRESS;
    return l->priority > PRIO_MAX ? E_OUT_OF_RANGE : E_OK;
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
    struct job *j = job_create(top, l.memory, l.threads, l.priority, &rc);
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
    job_set_limits(j, l.memory, l.threads, l.priority);
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
    /* A call that woke or lifted a thread above this one (a reply, a lower
       priority of its own) lets it run now rather than at the slice's end. */
    sched_yield_if_outranked();
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
    ica->handles = 0; ica->handle_count = 0; ica->reply_handles = 0; ica->reply_handle_cap = 0;
    ica->reserved = 0;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    ica->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    /* So is each ipc_handle's, and the count, in a call and in a reply. */
    struct ipc_handle *ih = (struct ipc_handle *)(FT_INFO + 0x40);
    ih->handle = -1; ih->op = IPC_HANDLE_MOVE; ih->rights = IPC_RIGHTS_SAME; ih->reserved = 0;
    ica->reserved = 0; ica->handles = FT_INFO + 0x40; ica->handle_count = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    ih->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    ica->handle_count = IPC_HANDLES_MAX + 1;
    step++;
    if (syscall_dispatch(SYS_IPC_CALL, FT_IARGS, 0, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
    struct ipc_reply_args *ipa = (struct ipc_reply_args *)FT_IARGS;
    ipa->data = 0; ipa->len = 0; ipa->token = -1;
    ipa->handles = FT_INFO + 0x40; ipa->handle_count = 1;
    ih->reserved = 0;
    step++;
    if (syscall_dispatch(SYS_IPC_REPLY, FT_IARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
    ih->reserved = 1;
    step++;
    if (syscall_dispatch(SYS_IPC_REPLY, FT_IARGS, 0, 0, 0, 0) != E_INVALID_ARGS) goto done;
    ipa->handle_count = IPC_HANDLES_MAX + 1;
    step++;
    if (syscall_dispatch(SYS_IPC_REPLY, FT_IARGS, 0, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
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

/* A first touch of one of the current process's mappings (memman/vmregion.h). */
static int usermode_demand_fault(uintptr_t addr, int write, int exec) {
    return vm_fault(thread_current_pid(), addr, write, exec);
}

void usermode_register_fault_handler(void) {
    set_user_fault_hook(usermode_fault);
    set_demand_fault_hook(usermode_demand_fault);
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

    /* 13-14: an endpoint is the wrong type for OBJINFO_PROCESS, and a send
       handle has no signals to wait for even on a handle that may wait */
    eh = ep_create();
    step = 13; if (eh <= 0 || pt_info(eh) != E_WRONG_TYPE) goto done;
    int32_t sh = ep_mint(eh, 0, HRIGHT_SEND);
    struct kobj *eo = sh > 0 ? htab_ref(tab, sh, KOBJ_SEND, 0, 0) : 0;
    ew = eo ? htab_install(tab, eo, HRIGHT_WAIT, 0) : -1;
    if (eo) kobj_unref(eo);
    if (sh > 0) htab_close(tab, sh);
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
#define KT_INFO  (TEST_CODE_VIRT + 0x780)   /* the victim's ipc_msg_info */
#define KT_TINFO (TEST_CODE_VIRT + 0x7C0)   /* the test thread's */
#define KT_TBUF  (TEST_CODE_VIRT + 0x7F0)   /* and its receive buffer, 16 bytes */

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

    /* 3: receiving, and no longer waiting on the endpoint */
    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    h = ep ? kt_make(&tid) : -1;
    step = 3; if (h <= 0) goto done;
    v = ep_install_handle(tid, ep, HRIGHT_RECV, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    struct ipc_recv_args *ra = (struct ipc_recv_args *)user_ptr(KT_ARGS);
    ra->cap = 0; ra->buf = 0; ra->info = KT_INFO; ra->ep_handle = v; ra->handles = 0; ra->handle_cap = 0;
    kt_call(SYS_IPC_RECV, KT_ARGS, 0, 0, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1) || kobj_waiters(&ep->hdr) != 0) goto done;
    htab_close(tab, h); h = -1;

    /* 4: calling, before anyone took the request: it is withdrawn, and its
       token with it */
    struct ipc_call_args *ca = (struct ipc_call_args *)user_ptr(KT_ARGS);
    *(volatile uint8_t *)user_ptr(KT_REQ) = 0x5A;
    uint32_t calls0 = kobj_live(KOBJ_REPLY);
    h = kt_make(&tid);
    step = 4; if (h <= 0) goto done;
    v = ep_install_send(tid, ep, 0, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    ca->req_len = 1; ca->reply_cap = 0; ca->req = KT_REQ; ca->reply = 0;
    ca->ep_handle = v; ca->reserved = 0;
    ca->handles = 0; ca->handle_count = 0; ca->reply_handles = 0; ca->reply_handle_cap = 0;
    kt_call(SYS_IPC_CALL, KT_ARGS, 0, 0, 0, 0);
    thread_start(tid);
    if (!kt_kill(h, tid, 1) || ep_queued(ep) != 0 || kobj_live(KOBJ_REPLY) != calls0
        || syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)eh, SIGNAL_EP_READABLE, 0, 0, 0) != E_TIMED_OUT) goto done;
    htab_close(tab, h); h = -1;

    /* 5-6: calling, with the request taken: the call stays with its token
       until the reply, which finds the caller gone and closes the token */
    h = kt_make(&tid);
    step = 5; if (h <= 0) goto done;
    v = ep_install_send(tid, ep, 0, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(tid); goto done; }
    ca->ep_handle = v;
    kt_call(SYS_IPC_CALL, KT_ARGS, 0, 0, 0, 0);
    thread_start(tid);
    struct ipc_recv_args *tr = (struct ipc_recv_args *)user_ptr(KT_TARGS);
    struct ipc_msg_info *ti = (struct ipc_msg_info *)user_ptr(KT_TINFO);
    tr->cap = 1; tr->buf = KT_TBUF; tr->info = KT_TINFO; tr->ep_handle = eh; tr->handles = 0; tr->handle_cap = 0;
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT
        || syscall_dispatch(SYS_IPC_RECV, KT_TARGS, 0, 0, 0, 0) != 1 || ti->reply <= 0) goto done;
    int32_t tok = ti->reply;
    if (!kt_kill(h, tid, 1) || kobj_live(KOBJ_REPLY) != calls0 + 1) goto done;
    struct ipc_reply_args *rp = (struct ipc_reply_args *)user_ptr(KT_TARGS);
    rp->data = KT_REQ; rp->len = 0; rp->token = tok; rp->handles = 0; rp->handle_count = 0;
    step = 6; if (syscall_dispatch(SYS_IPC_REPLY, KT_TARGS, 0, 0, 0, 0) != E_PEER_CLOSED
                  || kobj_live(KOBJ_REPLY) != calls0
                  || syscall_dispatch(SYS_IPC_REPLY, KT_TARGS, 0, 0, 0, 0) != E_BAD_HANDLE) goto done;
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
    r->cap = cap; r->buf = ST_TBUF; r->info = ST_TSEND; r->ep_handle = eh; r->handles = 0; r->handle_cap = 0;
    return syscall_dispatch(SYS_IPC_RECV, KT_TARGS, 0, 0, 0, 0);
}

/* It answers the call behind the token it last received. */
static int st_reply(void) {
    struct ipc_reply_args *r = (struct ipc_reply_args *)user_ptr(KT_TARGS);
    int32_t tok = ((const struct ipc_msg_info *)user_ptr(ST_TSEND))->reply;
    r->data = KT_REQ; r->len = 0; r->token = tok; r->handles = 0; r->handle_count = 0;
    return syscall_dispatch(SYS_IPC_REPLY, KT_TARGS, 0, 0, 0, 0);
}

/* What was just received is a stop notice from `by` giving `ms`: from the
   kernel, with no badge and no token. */
static int st_is_notice(int rc, const void *buf, const void *info, uint32_t by, uint32_t ms) {
    const struct stop_notice *n = buf;
    const struct ipc_msg_info *i = info;
    return rc == (int)sizeof *n && i->sender == IPC_SENDER_KERNEL && i->reply == 0
        && i->badge == 0 && i->len == sizeof *n && i->reply_cap == 0
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
    ra->cap = sizeof *note; ra->buf = ST_NOTE; ra->info = ST_SENDER; ra->ep_handle = v; ra->handles = 0; ra->handle_cap = 0;
    ((volatile struct ipc_msg_info *)user_ptr(ST_SENDER))->sender = 0;
    st_call(0, SYS_PROCESS_STOP_ENDPOINT, (uint32_t)v, 0);
    st_call(1, SYS_SLEEP, 1, 0);
    st_call(2, SYS_IPC_RECV, KT_ARGS, 0);
    thread_start(tid);
    if (pt_wait(h, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT || !thread_is_blocked(tid)
        || kobj_waiters(&ep->hdr) != 1) goto done;
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
                  || ep->stop_pending || ep_queued(ep) != 0) goto done;

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

    /* 12-13: a notice ahead of a queued call: received first, with no token
       to answer, and the call stays queued behind it. Then the call is
       received as usual, and a reply after its caller is killed finds it
       gone. */
    ha = kt_make(&tida);
    step = 12; if (ha <= 0) goto done;
    v = ep_install_send(tida, ep, 0, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(tida); goto done; }
    struct ipc_call_args *ca = (struct ipc_call_args *)user_ptr(KT_ARGS);
    const struct ipc_msg_info *ti = tsend;
    *(volatile uint8_t *)user_ptr(KT_REQ) = 0x5A;
    ca->req_len = 1; ca->reply_cap = 0; ca->req = KT_REQ; ca->reply = 0;
    ca->ep_handle = v; ca->reserved = 0;
    ca->handles = 0; ca->handle_count = 0; ca->reply_handles = 0; ca->reply_handle_cap = 0;
    kt_call(SYS_IPC_CALL, KT_ARGS, 0, 0, 0, 0);
    thread_start(tida);
    if (pt_wait(ha, SIGNAL_PROCESS_ENDED, 50, 0) != E_TIMED_OUT || ep_queued(ep) != 1
        || !thread_is_blocked(tida)) goto done;
    if (ep_post_stop(ep, 77, 5) != 0
        || !st_is_notice(st_recv(eh, 64), tbuf, tsend, 77, 5) || ep_queued(ep) != 1
        || st_reply() != E_BAD_HANDLE || !thread_is_blocked(tida)) goto done;
    step = 13; if (st_recv(eh, 64) != 1 || *(const uint8_t *)tbuf != 0x5A
                   || ti->sender != thread_pid(tida) || ti->reply <= 0 || ti->badge != 0
                   || ti->len != 1 || ti->reply_cap != 0 || ep_queued(ep) != 0) goto done;
    if (!kt_kill(ha, tida, 1) || st_reply() != E_PEER_CLOSED) goto done;
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

static int jt_create(int32_t parent, uint64_t mem, uint32_t threads, uint32_t prio) {
    struct job_limits *l = (struct job_limits *)user_ptr(JT_LIM);
    l->memory = mem; l->threads = threads; l->priority = prio;
    return syscall_dispatch(SYS_JOB_CREATE, (uint32_t)parent, JT_LIM, 0, 0, 0);
}

static int jt_limits(int32_t h, uint64_t mem, uint32_t threads, uint32_t prio) {
    struct job_limits *l = (struct job_limits *)user_ptr(JT_LIM);
    l->memory = mem; l->threads = threads; l->priority = prio;
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
    step = 1; if (jt_create(9999, 0, 0, 0) != E_BAD_HANDLE || jt_create(0, 0, 0, PRIO_MAX + 1) != E_OUT_OF_RANGE
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
    if (jt_limits(j, 2u * P, 2, PRIO_MAX + 1) != E_OUT_OF_RANGE || jt_limits(h, 0, 0, 0) != E_WRONG_TYPE
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

    /* 1-2: the clock: the kernel's own, read between two reads of it (to the
       TSC's grain since 1.2c, not whole ticks), and it measures a sleep */
    *now = 1;
    uint64_t k0 = timer_now_ns();
    step = 1; if (syscall_dispatch(SYS_CLOCK_MONOTONIC, TT_NOW, 0, 0, 0, 0) != E_OK
                  || *now == 1 || *now < k0
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

/* ---- queue objects (Phase 1.2b) --------------------------------------------
 * Through the dispatcher over a user mapping, with timers (signalled from the
 * tick) and a job (from a syscall) as the objects waited for. Keys use their
 * high halves, so a decoder that drops one fails. Nothing logs between taking
 * a time and checking it. 1 = pass, 0 = fail. */
#define QT_EV    (TEST_CODE_VIRT + 0x400)   /* QUEUE_EVENTS_MAX struct queue_event */
#define QT_GUARD (QT_EV + QUEUE_EVENTS_MAX * sizeof(struct queue_event))   /* a u64 after it */

static int qt_reg(int32_t q, int32_t h, uint32_t sig, uint64_t key) {
    return um_call(SYS_QUEUE_REGISTER, 1u << 3, (uint32_t)q, (uint32_t)h, sig, key, 0);
}
static int qt_arm(int32_t q, uint64_t key) { return um_call(SYS_QUEUE_ARM, 1u << 1, (uint32_t)q, key, 0, 0, 0); }
static int qt_unreg(int32_t q, uint64_t key) { return um_call(SYS_QUEUE_UNREGISTER, 1u << 1, (uint32_t)q, key, 0, 0, 0); }
static int qt_wait(int32_t q, uint32_t n, uint64_t deadline) {
    return um_call(SYS_QUEUE_WAIT, 1u << 3, (uint32_t)q, QT_EV, n, deadline, 0);
}
/* Is a key ready? SYS_OBJECT_WAIT on the queue only looks. */
static int qt_ready(int32_t q) {
    return syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)q, SIGNAL_QUEUE_READY, 0, 0, 0) == E_OK;
}

int usermode_queue_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    volatile struct queue_event *ev = (volatile struct queue_event *)user_ptr(QT_EV);
    volatile uint64_t *guard = (volatile uint64_t *)user_ptr(QT_GUARD);
    uint32_t qlive0 = kobj_live(KOBJ_QUEUE), tlive0 = kobj_live(KOBJ_TIMER);
    uint32_t regs0 = kqueue_reg_count(), armed0 = ktimer_armed_count(), jobs0 = job_live();
    const uint64_t K1 = 0x1111222233334444ull, K2 = 0xAAAA0000BBBB0002ull, K3 = 0x300000000ull;
    int32_t q = -1, t = -1, u = -1, v = -1, d = -1, eh = -1, j = -1;
    int step = 0, ok = 0;

    /* 1: a new queue, with the rights it is made with, and nothing ready */
    q = syscall_dispatch(SYS_QUEUE_CREATE, 0, 0, 0, 0, 0);
    struct object_info_args *oi = (struct object_info_args *)user_ptr(TT_IARGS);
    struct object_basic *ob = (struct object_basic *)user_ptr(TT_INFO);
    oi->cap = sizeof *ob; oi->buf = TT_INFO; oi->handle = q; oi->topic = OBJINFO_BASIC;
    step = 1; if (q <= 0 || syscall_dispatch(SYS_OBJECT_INFO, TT_IARGS, 0, 0, 0, 0) != (int)sizeof *ob
                  || ob->kind != OBJ_TYPE_QUEUE
                  || ob->rights != (HRIGHT_WAIT | HRIGHT_REGISTER | HRIGHT_DUPLICATE)
                  || kobj_live(KOBJ_QUEUE) != qlive0 + 1
                  || qt_wait(q, 1, 0) != E_TIMED_OUT || qt_ready(q)) goto done;

    /* 2: a registered timer's key comes back when it fires, not before, once,
       with the signals it fired with; the wait blocked until then */
    t = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    u = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    for (uint32_t i = 0; i < QUEUE_EVENTS_MAX; i++) { ev[i].key = ~0ull; ev[i].signals = ~0u; ev[i].reserved = ~0u; }
    uint64_t dl = timer_now_ns() + TT_MS(30);
    step = 2; if (t <= 0 || u <= 0 || qt_reg(q, t, SIGNAL_TIMER_FIRED, K1) != E_OK
                  || kqueue_reg_count() != regs0 + 1
                  || tt_set(t, dl) != E_OK || qt_wait(q, 4, 0) != E_TIMED_OUT
                  || qt_wait(q, 4, dl + TT_MS(2000)) != 1 || timer_now_ns() < dl
                  || ev[0].key != K1 || ev[0].signals != SIGNAL_TIMER_FIRED || ev[0].reserved != 0
                  || ev[1].key != ~0ull || qt_ready(q)) goto done;

    /* 3: one-shot: the timer fires again, and the key does not come back */
    step = 3; if (tt_set(t, 0) != E_OK || qt_wait(q, 4, 0) != E_TIMED_OUT || qt_ready(q)) goto done;

    /* 4: re-arming looks at the signal as it is: fired while idle, it is ready
       at once; arming a ready key leaves it ready, once */
    step = 4; if (qt_arm(q, K1) != E_OK || !qt_ready(q) || qt_arm(q, K1) != E_OK
                  || qt_wait(q, 4, 0) != 1 || ev[0].key != K1
                  || qt_wait(q, 4, 0) != E_TIMED_OUT) goto done;

    /* 5: a signal set before registering makes the key ready at once */
    tt_set(u, 0);
    step = 5; if (qt_reg(q, u, SIGNAL_TIMER_FIRED, K2) != E_OK
                  || qt_wait(q, 4, 0) != 1 || ev[0].key != K2) goto done;

    /* 6: a wait returns only the keys that became ready: u at 20 ms comes back
       alone, while t (40 ms) has not fired; then t */
    uint64_t base = timer_now_ns();
    tt_set(t, base + TT_MS(40));
    tt_set(u, base + TT_MS(20));
    step = 6; if (qt_arm(q, K1) != E_OK || qt_arm(q, K2) != E_OK
                  || qt_wait(q, 4, base + TT_MS(2000)) != 1 || ev[0].key != K2
                  || tt_look(t) != E_TIMED_OUT
                  || qt_wait(q, 4, base + TT_MS(2000)) != 1 || ev[0].key != K1
                  || timer_now_ns() < base + TT_MS(40)) goto done;

    /* 7: keys ready together come back together, in the order they became
       ready; a count of 1 takes one and leaves the other ready */
    step = 7; if (qt_arm(q, K1) != E_OK || qt_arm(q, K2) != E_OK
                  || qt_wait(q, 1, 0) != 1 || ev[0].key != K1 || !qt_ready(q)
                  || qt_wait(q, 4, 0) != 1 || ev[0].key != K2 || qt_ready(q)
                  || qt_arm(q, K2) != E_OK || qt_arm(q, K1) != E_OK
                  || qt_wait(q, 4, 0) != 2 || ev[0].key != K2 || ev[1].key != K1) goto done;

    /* 8: an armed key unregistered is not reported when its object fires */
    base = timer_now_ns();
    tt_set(t, base + TT_MS(20));
    step = 8; if (qt_arm(q, K1) != E_OK || qt_unreg(q, K1) != E_OK
                  || kqueue_reg_count() != regs0 + 1
                  || qt_wait(q, 4, base + TT_MS(50)) != E_TIMED_OUT || tt_look(t) != E_OK) goto done;

    /* 9: a ready key unregistered leaves the ready list, and the queue's
       signal with it */
    step = 9; if (qt_arm(q, K2) != E_OK || !qt_ready(q) || qt_unreg(q, K2) != E_OK
                  || qt_ready(q) || qt_wait(q, 4, 0) != E_TIMED_OUT
                  || kqueue_reg_count() != regs0) goto done;

    /* 10: another type, signalled from a syscall: an empty job, killed */
    j = jt_create(0, 0, 0, 0);
    step = 10; if (j <= 0 || qt_reg(q, j, SIGNAL_JOB_ENDED, K3) != E_OK || qt_wait(q, 4, 0) != E_TIMED_OUT
                   || jt_kill(j) != E_OK || qt_wait(q, 4, 0) != 1 || ev[0].key != K3
                   || !(ev[0].signals & SIGNAL_JOB_ENDED) || qt_unreg(q, K3) != E_OK) goto done;
    htab_close(tab, j); j = -1;

    /* 11-13: refusals, each its own error, none leaving a registration */
    step = 11; if (qt_reg(q, t, SIGNAL_TIMER_FIRED, K1) != E_OK   /* t fired: ready */
                   || qt_reg(q, u, SIGNAL_TIMER_FIRED, K1) != E_ALREADY_EXISTS
                   || qt_arm(q, K2) != E_NOT_FOUND || qt_unreg(q, K2) != E_NOT_FOUND
                   || qt_reg(q, t, 0, K2) != E_INVALID_ARGS
                   || qt_reg(q, t, SIGNAL_TIMER_FIRED << 1, K2) != E_INVALID_ARGS
                   || qt_wait(q, 0, 0) != E_INVALID_ARGS
                   || um_call(SYS_QUEUE_WAIT, 1u << 3, (uint32_t)q, USER_VLIMIT, 1, 0, 0) != E_BAD_ADDRESS
                   || kqueue_reg_count() != regs0 + 1) goto done;
    /* a send handle has no signals, even on a handle that may wait */
    eh = ep_create();
    int32_t sh = eh > 0 ? ep_mint(eh, 0, HRIGHT_SEND) : -1;
    struct kobj *eo = sh > 0 ? htab_ref(tab, sh, KOBJ_SEND, 0, 0) : 0;
    int32_t ew = eo ? htab_install(tab, eo, HRIGHT_WAIT, 0) : -1;
    if (eo) kobj_unref(eo);
    if (sh > 0) htab_close(tab, sh);
    htab_close(tab, eh);
    eh = ew;
    step = 12; if (eh <= 0 || qt_reg(9999, t, SIGNAL_TIMER_FIRED, K2) != E_BAD_HANDLE
                   || qt_reg(q, 9999, SIGNAL_TIMER_FIRED, K2) != E_BAD_HANDLE
                   || qt_wait(9999, 1, 0) != E_BAD_HANDLE
                   || qt_arm(t, K1) != E_WRONG_TYPE || qt_wait(t, 1, 0) != E_WRONG_TYPE
                   || qt_reg(t, u, SIGNAL_TIMER_FIRED, K2) != E_WRONG_TYPE
                   || qt_reg(q, q, SIGNAL_QUEUE_READY, K2) != E_WRONG_TYPE
                   || qt_reg(q, eh, 1, K2) != E_NOT_SUPPORTED
                   || kqueue_reg_count() != regs0 + 1) goto done;
    /* each right is needed: REGISTER to register, arm, unregister; WAIT to
       wait; the object's WAIT to register it */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)q, HRIGHT_WAIT, 0, 0, 0);
    step = 13; if (d <= 0 || qt_reg(d, u, SIGNAL_TIMER_FIRED, K2) != E_ACCESS_DENIED
                   || qt_arm(d, K1) != E_ACCESS_DENIED || qt_unreg(d, K1) != E_ACCESS_DENIED
                   || qt_wait(d, 1, 0) != 1 || ev[0].key != K1) goto done;
    htab_close(tab, d);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)q, HRIGHT_REGISTER, 0, 0, 0);
    step = 14; if (d <= 0 || qt_arm(d, K1) != E_OK || qt_wait(d, 1, 0) != E_ACCESS_DENIED
                   || qt_wait(q, 1, 0) != 1 || ev[0].key != K1) goto done;
    htab_close(tab, d);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)u, HRIGHT_ARM, 0, 0, 0);
    step = 15; if (d <= 0 || qt_reg(q, d, SIGNAL_TIMER_FIRED, K2) != E_ACCESS_DENIED
                   || kqueue_reg_count() != regs0 + 1) goto done;
    htab_close(tab, d); d = -1;

    /* 16: QUEUE_REGISTER_MAX registrations and no more; a count past
       QUEUE_EVENTS_MAX takes that many, oldest first, and writes no further */
    int filled = 1;
    for (uint32_t i = 0; i + 1 < QUEUE_REGISTER_MAX; i++)   /* K1 is one */
        if (qt_reg(q, t, SIGNAL_TIMER_FIRED, 100 + i) != E_OK) filled = 0;
    *guard = 0x5A5A5A5A5A5A5A5Aull;
    step = 16; if (!filled || qt_reg(q, u, SIGNAL_TIMER_FIRED, K2) != E_NO_RESOURCES
                   || qt_wait(q, 100, 0) != (int)QUEUE_EVENTS_MAX
                   || ev[0].key != 100 || ev[QUEUE_EVENTS_MAX - 1].key != 100 + QUEUE_EVENTS_MAX - 1
                   || *guard != 0x5A5A5A5A5A5A5A5Aull || !qt_ready(q)) goto done;
    for (uint32_t i = 0; i + 1 < QUEUE_REGISTER_MAX; i++)
        if (qt_unreg(q, 100 + i) != E_OK) filled = 0;
    step = 16; if (!filled || kqueue_reg_count() != regs0 + 1 || qt_ready(q)) goto done;

    /* 17: a registration holds its object: a timer whose only handle is closed
       still fires into the queue */
    v = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    dl = timer_now_ns() + TT_MS(20);
    step = 17; if (v <= 0 || qt_reg(q, v, SIGNAL_TIMER_FIRED, K2) != E_OK || tt_set(v, dl) != E_OK) goto done;
    htab_close(tab, v); v = -1;
    step = 17; if (kobj_live(KOBJ_TIMER) != tlive0 + 3
                   || qt_wait(q, 4, dl + TT_MS(2000)) != 1 || ev[0].key != K2 || timer_now_ns() < dl) goto done;

    /* 18: the queue's last handle lets go of every object it held */
    htab_close(tab, t); t = -1;
    htab_close(tab, u); u = -1;
    step = 18; if (kobj_live(KOBJ_TIMER) != tlive0 + 2) goto done;   /* K1 holds t, K2 v */
    htab_close(tab, q); q = -1;
    step = 18; if (kobj_live(KOBJ_QUEUE) != qlive0 || kobj_live(KOBJ_TIMER) != tlive0
                   || kqueue_reg_count() != regs0 || ktimer_armed_count() != armed0
                   || job_live() != jobs0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "queue test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (q > 0)  htab_close(tab, q);
    if (t > 0)  htab_close(tab, t);
    if (u > 0)  htab_close(tab, u);
    if (v > 0)  htab_close(tab, v);
    if (d > 0)  htab_close(tab, d);
    if (eh > 0) htab_close(tab, eh);
    if (j > 0)  htab_close(tab, j);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- notifications (Phase 1.3c) --------------------------------------------
 * Through the dispatcher over a user mapping. Bits use the word's high half,
 * so a decoder that drops it fails. A helper thread signals the notification
 * (through the kernel: it has a handle table of its own) while this one is
 * blocked, which is the wake a signal exists for. 1 = pass, 0 = fail. */
#define NT_BITS  (TEST_CODE_VIRT + 0x100)   /* SYS_NOTIFY_TAKE's *u64 */
#define NT_INFO  (TEST_CODE_VIRT + 0x200)   /* struct object_basic */
#define NT_IARGS (TEST_CODE_VIRT + 0x300)   /* struct object_info_args */
#define NT_HIGH  (1ull << 63)

static volatile int     nt_go, nt_stage, nt_rc, nt_main;
static struct knotify *volatile nt_obj;   /* a reference: the helper has its own handle table */

static int nt_mint(int32_t n, uint64_t bits, uint32_t rights) {
    return um_call(SYS_NOTIFY_MINT, 1u << 1, (uint32_t)n, bits, rights, 0, 0);
}
static int nt_signal(int32_t h, uint64_t bits) {
    return um_call(SYS_NOTIFY_SIGNAL, 1u << 1, (uint32_t)h, bits, 0, 0, 0);
}
static int nt_take(int32_t n) { return syscall_dispatch(SYS_NOTIFY_TAKE, (uint32_t)n, NT_BITS, 0, 0, 0); }
static int nt_look(int32_t n) {
    return syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)n, SIGNAL_NOTIFY_PENDING, 0, 0, 0);
}
/* The kind and rights object_info gives for `h`. */
static int nt_basic(int32_t h, uint32_t kind, uint32_t rights) {
    struct object_info_args *oi = (struct object_info_args *)user_ptr(NT_IARGS);
    const struct object_basic *ob = (const struct object_basic *)user_ptr(NT_INFO);
    oi->cap = sizeof *ob; oi->buf = NT_INFO; oi->handle = h; oi->topic = OBJINFO_BASIC;
    return syscall_dispatch(SYS_OBJECT_INFO, NT_IARGS, 0, 0, 0, 0) == (int)sizeof *ob
        && ob->kind == kind && ob->rights == rights;
}

/* Signals once the test thread is blocked on the notification (or gives up
   waiting for that and signals anyway, so the test cannot hang on it). */
static void nt_helper(void) {
    while (!nt_go) yield();
    for (int i = 0; i < 500 && !thread_is_blocked(nt_main); i++) yield();
    nt_stage = thread_is_blocked(nt_main) ? 1 : -1;
    nt_rc = knotify_signal(nt_obj, NT_HIGH);
    nt_stage = nt_stage == 1 ? 2 : -2;
}

int usermode_notify_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    volatile uint64_t *bits = (volatile uint64_t *)user_ptr(NT_BITS);
    uint32_t nlive0 = kobj_live(KOBJ_NOTIFY), slive0 = kobj_live(KOBJ_SIGNALLER);
    uint32_t regs0 = kqueue_reg_count();
    int32_t n = -1, s = -1, d = -1, q = -1, t = -1;
    int tid = -1, step = 0, ok = 0;

    /* 1-2: a new notification, with the rights it is made with; nothing set,
       and a take finds nothing */
    n = syscall_dispatch(SYS_NOTIFY_CREATE, 0, 0, 0, 0, 0);
    step = 1; if (n <= 0 || !nt_basic(n, OBJ_TYPE_NOTIFY, HRIGHT_WAIT | HRIGHT_SIGNAL | HRIGHT_RECV | HRIGHT_DUPLICATE)
                  || kobj_live(KOBJ_NOTIFY) != nlive0 + 1 || nt_look(n) != E_TIMED_OUT) goto done;
    *bits = 0xA5A5A5A5A5A5A5A5ull;
    step = 2; if (nt_take(n) != E_OK || *bits != 0) goto done;

    /* 3: a signal sets PENDING; a take hands back every bit, high half too,
       and clears the word and the signal */
    step = 3; if (nt_signal(n, 1) != E_OK || nt_signal(n, NT_HIGH) != E_OK || nt_look(n) != E_OK
                  || nt_take(n) != E_OK || *bits != (NT_HIGH | 1) || nt_look(n) != E_TIMED_OUT
                  || nt_take(n) != E_OK || *bits != 0) goto done;

    /* 4: signals before a take combine: three of one bit and one of another
       are two bits */
    nt_signal(n, 2); nt_signal(n, 2); nt_signal(n, 2);
    step = 4; if (nt_signal(n, 4) != E_OK || nt_take(n) != E_OK || *bits != 6) goto done;

    /* 5-6: a signaller sets its own bits and no others; a refused signal sets
       nothing */
    s = nt_mint(n, 0x10 | (1ull << 40), HRIGHT_SIGNAL | HRIGHT_DUPLICATE);
    step = 5; if (s <= 0 || !nt_basic(s, OBJ_TYPE_SIGNALLER, HRIGHT_SIGNAL | HRIGHT_DUPLICATE)
                  || kobj_live(KOBJ_SIGNALLER) != slive0 + 1
                  || nt_signal(s, 1ull << 40) != E_OK || nt_signal(s, 0x10) != E_OK
                  || nt_take(n) != E_OK || *bits != (0x10 | (1ull << 40))) goto done;
    step = 6; if (nt_signal(s, 0x20) != E_ACCESS_DENIED || nt_signal(s, 0x10 | 0x20) != E_ACCESS_DENIED
                  || nt_signal(s, NT_HIGH) != E_ACCESS_DENIED || nt_look(n) != E_TIMED_OUT) goto done;

    /* 7-9: refusals, each its own error */
    t = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    step = 7; if (t <= 0 || nt_mint(n, 0, HRIGHT_SIGNAL) != E_INVALID_ARGS
                  || nt_mint(n, 1, HRIGHT_DUPLICATE) != E_ACCESS_DENIED
                  || nt_mint(n, 1, HRIGHT_SIGNAL | HRIGHT_RECV) != E_ACCESS_DENIED
                  || nt_mint(n, 1, HRIGHT_SIGNAL | HRIGHT_WAIT) != E_ACCESS_DENIED
                  || nt_mint(s, 1, HRIGHT_SIGNAL) != E_WRONG_TYPE || nt_mint(t, 1, HRIGHT_SIGNAL) != E_WRONG_TYPE
                  || nt_mint(9999, 1, HRIGHT_SIGNAL) != E_BAD_HANDLE
                  || kobj_live(KOBJ_SIGNALLER) != slive0 + 1) goto done;
    step = 8; if (nt_signal(n, 0) != E_INVALID_ARGS || nt_signal(s, 0) != E_INVALID_ARGS
                  || nt_signal(t, 1) != E_WRONG_TYPE || nt_signal(9999, 1) != E_BAD_HANDLE
                  || nt_take(s) != E_WRONG_TYPE || nt_take(t) != E_WRONG_TYPE || nt_take(9999) != E_BAD_HANDLE
                  || syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)s, SIGNAL_NOTIFY_PENDING, 0, 0, 0) != E_ACCESS_DENIED
                  || syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)n, SIGNAL_NOTIFY_PENDING << 1, 0, 0, 0) != E_INVALID_ARGS
                  || nt_look(n) != E_TIMED_OUT) goto done;
    /* each right is needed: no SIGNAL, no signal; no RECV, no take or mint */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)n, HRIGHT_WAIT | HRIGHT_RECV, 0, 0, 0);
    step = 9; if (d <= 0 || nt_signal(d, 1) != E_ACCESS_DENIED || nt_look(n) != E_TIMED_OUT) goto done;
    htab_close(tab, d);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)n, HRIGHT_WAIT | HRIGHT_SIGNAL, 0, 0, 0);
    step = 10; if (d <= 0 || nt_signal(d, 8) != E_OK || nt_take(d) != E_ACCESS_DENIED
                   || nt_mint(d, 1, HRIGHT_SIGNAL) != E_ACCESS_DENIED || nt_look(n) != E_OK) goto done;
    htab_close(tab, d); d = -1;
    /* a signaller without SIGNAL (a copy made with DUPLICATE alone) signals nothing */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)s, HRIGHT_DUPLICATE, 0, 0, 0);
    step = 11; if (d <= 0 || nt_signal(d, 0x10) != E_ACCESS_DENIED) goto done;
    htab_close(tab, d); d = -1;

    /* 12: a take into a bad address takes nothing: the bits stay */
    step = 12; if (syscall_dispatch(SYS_NOTIFY_TAKE, (uint32_t)n, USER_VLIMIT, 0, 0, 0) != E_BAD_ADDRESS
                   || nt_look(n) != E_OK || nt_take(n) != E_OK || *bits != 8) goto done;

    /* 13: through a queue: a signal readies the key, with PENDING */
    q = syscall_dispatch(SYS_QUEUE_CREATE, 0, 0, 0, 0, 0);
    const struct queue_event *ev = (const struct queue_event *)user_ptr(QT_EV);
    step = 13; if (q <= 0 || qt_reg(q, n, SIGNAL_NOTIFY_PENDING, 0xC0DE000000000013ull) != E_OK
                   || qt_wait(q, 1, 0) != E_TIMED_OUT || nt_signal(s, 0x10) != E_OK
                   || qt_wait(q, 1, 0) != 1 || ev[0].key != 0xC0DE000000000013ull
                   || ev[0].signals != SIGNAL_NOTIFY_PENDING
                   || nt_take(n) != E_OK || *bits != 0x10) goto done;

    /* 14: a signal from another thread wakes this one, blocked on it */
    nt_go = 0; nt_stage = 0; nt_rc = 1; nt_main = thread_current_id();
    nt_obj = (struct knotify *)htab_ref(tab, n, KOBJ_NOTIFY, 0, 0);
    step = 14; if (!nt_obj) goto done;
    tid = thread_create("ktnotify", nt_helper);
    if (tid < 0) { kobj_unref(&nt_obj->hdr); goto done; }
    nt_go = 1;
    int wrc = syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)n, SIGNAL_NOTIFY_PENDING,
                               timer_now_ns() + 2000ull * TIMER_NS_PER_TICK, 0, 0);
    for (int i = 0; i < 200 && nt_stage < 2 && nt_stage > -2; i++) yield();
    tid = -1;   /* it has run to its end, or will without us */
    if (nt_stage == 2 || nt_stage == -2) kobj_unref(&nt_obj->hdr);   /* else it may still use it: a leak, and red */
    step = 15; if (wrc != E_OK || nt_stage != 2 || nt_rc != E_OK
                   || nt_take(n) != E_OK || *bits != NT_HIGH) goto done;

    /* 16-17: the notification lasts while a handle on it does; after the last
       closes, a signaller's signal is E_PEER_CLOSED, and the signaller still
       holds the object until it goes too */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)n, HRIGHT_WAIT, 0, 0, 0);
    htab_close(tab, q); q = -1;
    htab_close(tab, n); n = -1;
    step = 16; if (d <= 0 || nt_signal(s, 0x10) != E_OK) goto done;
    htab_close(tab, d); d = -1;
    step = 17; if (nt_signal(s, 0x10) != E_PEER_CLOSED || kobj_live(KOBJ_NOTIFY) != nlive0 + 1) goto done;
    htab_close(tab, s); s = -1;
    step = 18; if (kobj_live(KOBJ_NOTIFY) != nlive0 || kobj_live(KOBJ_SIGNALLER) != slive0
                   || kqueue_reg_count() != regs0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "notify test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (tid >= 0) { nt_go = 1; for (int i = 0; i < 1000 && nt_stage < 2 && nt_stage > -2; i++) yield(); }
    if (q > 0) htab_close(tab, q);
    if (n > 0) htab_close(tab, n);
    if (s > 0) htab_close(tab, s);
    if (d > 0) htab_close(tab, d);
    if (t > 0) htab_close(tab, t);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- pipes (Phase 1.4a) ----------------------------------------------------
 * Through the dispatcher over a user mapping, with the test thread holding no
 * GRANT_DISK: a pipe's handles are the authority for SYS_FILE_READ and
 * SYS_FILE_WRITE on it. A helper thread closes the other end while this one
 * is blocked on a read or a write, which is the wake a closed end exists
 * for. 1 = pass, 0 = fail. */
#define PT_ENDS  (TEST_CODE_VIRT + 0x000)   /* SYS_PIPE_CREATE's i32[2] */
#define PT_INFO  (TEST_CODE_VIRT + 0x100)   /* struct object_basic */
#define PT_IARGS (TEST_CODE_VIRT + 0x200)   /* struct object_info_args */
#define PT_SRC   (TEST_CODE_VIRT + 0x400)   /* 1024 bytes written */
#define PT_DST   (TEST_CODE_VIRT + 0x800)   /* 2048 bytes read */

static volatile int     pt_go, pt_stage, pt_main;
static volatile int32_t pt_close;           /* the handle the helper closes */
static struct htab *volatile pt_tab;

static int pt_create(uint32_t size, int32_t *rd, int32_t *wr) {
    volatile int32_t *ends = (volatile int32_t *)user_ptr(PT_ENDS);
    ends[0] = ends[1] = -1;
    int rc = syscall_dispatch(SYS_PIPE_CREATE, size, PT_ENDS, 0, 0, 0);
    *rd = ends[0]; *wr = ends[1];
    return rc;
}
static int pt_read(int32_t h, uint32_t off, uint64_t n) {
    return syscall_dispatch(SYS_FILE_READ, (uint32_t)h, PT_DST + off, (uintptr_t)n, 0, 0);
}
static int pt_write(int32_t h, uint32_t off, uint64_t n) {
    return syscall_dispatch(SYS_FILE_WRITE, (uint32_t)h, PT_SRC + off, (uintptr_t)n, 0, 0);
}
/* Whether `h` has `sig` set now. */
static int pt_set(int32_t h, uint32_t sig) {
    return syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)h, sig, 0, 0, 0) == E_OK;
}
static int pt_basic(int32_t h, uint32_t rights) {
    struct object_info_args *oi = (struct object_info_args *)user_ptr(PT_IARGS);
    const struct object_basic *ob = (const struct object_basic *)user_ptr(PT_INFO);
    oi->cap = sizeof *ob; oi->buf = PT_INFO; oi->handle = h; oi->topic = OBJINFO_BASIC;
    return syscall_dispatch(SYS_OBJECT_INFO, PT_IARGS, 0, 0, 0, 0) == (int)sizeof *ob
        && ob->kind == OBJ_TYPE_PIPE && ob->rights == rights;
}
/* The bytes read at PT_DST + off are the pattern from `from` on. */
static int pt_pattern(uint32_t off, uint32_t n, uint32_t from) {
    const volatile uint8_t *d = (const volatile uint8_t *)user_ptr(PT_DST + off);
    for (uint32_t i = 0; i < n; i++)
        if (d[i] != (uint8_t)((from + i) * 7u + 3u)) return 0;
    return 1;
}

/* Closes pt_close once the test thread is blocked (or gives up waiting for
   that and closes it anyway, so the test cannot hang on it). */
static void pt_helper(void) {
    while (!pt_go) yield();
    for (int i = 0; i < 500 && !thread_is_blocked(pt_main); i++) yield();
    pt_stage = thread_is_blocked(pt_main) ? 1 : -1;
    htab_close(pt_tab, pt_close);
    pt_stage = pt_stage == 1 ? 2 : -2;
}

/* Run the helper against `h` while the test thread does `blocked`; 1 if the
   helper saw it blocked and finished. */
static int pt_helper_start(int32_t h) {
    pt_go = 0; pt_stage = 0; pt_main = thread_current_id(); pt_close = h;
    if (thread_create("ktpipe", pt_helper) < 0) return 0;
    pt_go = 1;
    return 1;
}
static int pt_helper_done(void) {
    for (int i = 0; i < 1000 && pt_stage < 2 && pt_stage > -2; i++) yield();
    return pt_stage == 2;
}

int usermode_pipe_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    int me = thread_current_id();
    struct htab *tab = thread_htab(me);
    struct job *job = thread_proc(me)->job;
    uint32_t caps0 = thread_current_caps();
    uint32_t live0 = kobj_live(KOBJ_PIPE);
    uint64_t used0 = job->mem_used;
    volatile uint8_t *src = (volatile uint8_t *)user_ptr(PT_SRC);
    volatile uint8_t *dst = (volatile uint8_t *)user_ptr(PT_DST);
    for (uint32_t i = 0; i < 1024; i++) src[i] = (uint8_t)(i * 7u + 3u);
    int32_t ar = -1, aw = -1, br = -1, bw = -1, d = -1;
    struct kpipe *bp = 0;
    int step = 0, ok = 0, rc;
    thread_set_caps(me, caps0 & ~GRANT_DISK);
    pt_go = 0;
    pt_tab = tab;

    /* 1: refusals make nothing: a size out of range either side, ends the
       caller cannot write */
    step = 1; if (pt_create(PIPE_SIZE_MIN - 1, &ar, &aw) != E_OUT_OF_RANGE
                  || pt_create(PIPE_SIZE_MAX + 1, &ar, &aw) != E_OUT_OF_RANGE
                  || syscall_dispatch(SYS_PIPE_CREATE, 0, USER_VLIMIT - 4, 0, 0, 0) != E_BAD_ADDRESS
                  || kobj_live(KOBJ_PIPE) != live0 || job->mem_used != used0) goto done;
    ar = aw = -1;

    /* 2: a pipe of the default size: two ends with their rights, the ring
       charged to the job; empty, so readable no, writable yes */
    step = 2; if (pt_create(0, &ar, &aw) != E_OK || ar <= 0 || aw <= 0
                  || !pt_basic(ar, HRIGHT_READ | HRIGHT_WAIT | HRIGHT_DUPLICATE)
                  || !pt_basic(aw, HRIGHT_WRITE | HRIGHT_WAIT | HRIGHT_DUPLICATE)
                  || kobj_live(KOBJ_PIPE) != live0 + 2 || job->mem_used != used0 + PIPE_SIZE_DEFAULT
                  || pt_set(ar, SIGNAL_PIPE_READABLE) || !pt_set(aw, SIGNAL_PIPE_WRITABLE)
                  || pt_set(ar, SIGNAL_PIPE_PEER_CLOSED) || pt_set(aw, SIGNAL_PIPE_PEER_CLOSED)) goto done;

    /* 3: bytes written are read back in order, without GRANT_DISK; a read
       takes what is there, no more, and the ring is readable while it holds
       any */
    step = 3; if (pt_write(aw, 0, 5) != 5 || !pt_set(ar, SIGNAL_PIPE_READABLE)
                  || pt_read(ar, 0, 64) != 5 || !pt_pattern(0, 5, 0)
                  || pt_set(ar, SIGNAL_PIPE_READABLE)) goto done;
    /* 4: each end does only its own half: no read on the write end, no write
       on the read end, nothing on a copy without the right; 0 bytes is 0.
       A byte waits throughout, so a read let through takes it rather than
       blocking on an empty pipe, and the one byte is all there is after. */
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)ar, HRIGHT_WAIT, 0, 0, 0);
    step = 4; if (d <= 0 || pt_write(aw, 0, 1) != 1
                  || pt_read(aw, 0, 1) != E_ACCESS_DENIED || pt_write(ar, 0, 1) != E_ACCESS_DENIED
                  || pt_read(d, 0, 1) != E_ACCESS_DENIED || pt_write(aw, 0, 0) != 0
                  || pt_read(ar, 0, 64) != 1 || !pt_pattern(0, 1, 0)
                  || pt_set(ar, SIGNAL_PIPE_READABLE)) goto done;
    htab_close(tab, d); d = -1;
    /* 5: a bad buffer takes nothing out and puts nothing in */
    step = 5; if (pt_write(aw, 0, 3) != 3
                  || syscall_dispatch(SYS_FILE_READ, (uint32_t)ar, USER_VLIMIT - 1, 2, 0, 0) != E_BAD_ADDRESS
                  || syscall_dispatch(SYS_FILE_WRITE, (uint32_t)aw, USER_VLIMIT - 1, 2, 0, 0) != E_BAD_ADDRESS
                  || pt_read(ar, 0, 64) != 3 || !pt_pattern(0, 3, 0)) goto done;

    /* 6-7: a small pipe filled to the brim is not writable; reading makes
       room, a write wraps round the ring's end, and the bytes come back in
       the order they went in */
    step = 6; if (pt_create(1024, &br, &bw) != E_OK || br <= 0 || bw <= 0
                  || pt_write(bw, 0, 1024) != 1024 || pt_set(bw, SIGNAL_PIPE_WRITABLE)
                  || pt_read(br, 0, 600) != 600 || !pt_pattern(0, 600, 0)
                  || !pt_set(bw, SIGNAL_PIPE_WRITABLE)) goto done;
    step = 7; if (pt_write(bw, 0, 600) != 600 || pt_read(br, 0, 2048) != 1024
                  || !pt_pattern(0, 424, 600) || !pt_pattern(424, 600, 0)
                  || pt_set(br, SIGNAL_PIPE_READABLE)) goto done;

    /* 8: a write of PIPE_ATOMIC bytes or fewer waits for room for all of it,
       never putting part: with 324 bytes free, 400 block, and when the
       reader goes the write fails having put none of them */
    bp = ((struct kpipe_end *)htab_ref(tab, bw, KOBJ_PIPE, 0, 0))->p;   /* the write end's reference */
    step = 8; if (pt_write(bw, 0, 700) != 700 || pt_set(bw, SIGNAL_PIPE_WRITABLE)
                  || !pt_helper_start(br)) goto done;
    rc = pt_write(bw, 0, 400);
    br = -1;   /* the helper closed it */
    step = 9; if (!pt_helper_done() || rc != E_PEER_CLOSED || kpipe_count(bp) != 700
                  || !pt_set(bw, SIGNAL_PIPE_PEER_CLOSED) || !pt_set(bw, SIGNAL_PIPE_WRITABLE)
                  || pt_write(bw, 0, 1) != E_PEER_CLOSED) goto done;

    /* 10: a reader blocked on an empty pipe wakes when the writer goes, to
       the end of the stream, and stays there */
    step = 10; if (!pt_helper_start(aw)) goto done;
    rc = pt_read(ar, 0, 64);
    aw = -1;
    step = 11; if (!pt_helper_done() || rc != 0 || !pt_set(ar, SIGNAL_PIPE_PEER_CLOSED)
                   || !pt_set(ar, SIGNAL_PIPE_READABLE) || pt_read(ar, 0, 64) != 0) goto done;
    htab_close(tab, ar); ar = -1;

    /* 12: bytes written before the writer went are still read, then the end */
    for (uint32_t i = 0; i < 16; i++) dst[i] = 0;
    step = 12; if (pt_create(PIPE_SIZE_MIN, &ar, &aw) != E_OK || pt_write(aw, 0, 9) != 9) goto done;
    htab_close(tab, aw); aw = -1;
    step = 13; if (pt_read(ar, 0, 64) != 9 || !pt_pattern(0, 9, 0) || pt_read(ar, 0, 64) != 0) goto done;
    htab_close(tab, ar); ar = -1;

    /* 14: the last handles gone, both pipes are, and their charge with them */
    kobj_unref(&bp->wr.hdr); bp = 0;
    htab_close(tab, bw); bw = -1;
    step = 14; if (kobj_live(KOBJ_PIPE) != live0 || job->mem_used != used0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "pipe test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (pt_go) pt_helper_done();   /* a helper still running finishes first */
    if (bp) kobj_unref(&bp->wr.hdr);
    if (ar > 0) htab_close(tab, ar);
    if (aw > 0) htab_close(tab, aw);
    if (br > 0) htab_close(tab, br);
    if (bw > 0) htab_close(tab, bw);
    if (d > 0)  htab_close(tab, d);
    thread_set_caps(me, caps0);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- memory objects (Phase 1.3d) -------------------------------------------
 * Through the dispatcher over a user mapping, with the test thread given a
 * memory account for the length of the test (as usermode_vm_test does). The
 * pages of a mapping arrive on the first touch: here through the
 * user-pointer checks, and from ring 3 in two real processes, one that reads
 * and stores and one that stores into a read-only view. Strict commit is
 * checked against a job's limit and against what memory can promise. Seals
 * (1.3e) bind every handle, and wait for the last writable view, including
 * one a process held until it ended. 1 = pass, 0 = fail. */
extern uint8_t user_blob_touch_start[], user_blob_touch_end[];

#define MT_ARGS  (TEST_CODE_VIRT + 0x000)   /* struct vm_map_args */
#define MT_INFO  (TEST_CODE_VIRT + 0x100)   /* struct object_basic / mem_info / process_info */
#define MT_IARGS (TEST_CODE_VIRT + 0x200)   /* struct object_info_args */
#define MT_VALUE 0x1122334455667788ull

static uint8_t  mt_image[KT_BLOB_MAX + 3 * 8];
static uint64_t mt_addrs[MMAP_MAX_REGIONS];

static int mt_create(uint64_t size, uint32_t flags) {
    return um_call(SYS_MEM_CREATE, 1u << 0, size, flags, 0, 0, 0);
}
static int mt_seal(int32_t h, uint32_t seals) {
    return syscall_dispatch(SYS_MEM_SEAL, (uint32_t)h, seals, 0, 0, 0);
}
/* object_info on `h` for `topic` into MT_INFO, `cap` bytes: the call's result. */
static int mt_info(int32_t h, uint32_t topic, uint32_t cap) {
    struct object_info_args *oi = (struct object_info_args *)user_ptr(MT_IARGS);
    oi->cap = cap; oi->buf = MT_INFO; oi->handle = h; oi->topic = topic;
    return syscall_dispatch(SYS_OBJECT_INFO, MT_IARGS, 0, 0, 0, 0);
}
/* SYS_VM_MAP of `obj` at `offset`, `length` bytes: the call's result; the
   address is in MT_ARGS. */
static int mt_map(int32_t obj, uint64_t offset, uint64_t length, uint32_t prot) {
    struct vm_map_args *a = (struct vm_map_args *)user_ptr(MT_ARGS);
    a->offset = offset; a->length = length; a->granted = 0; a->addr = 0;
    a->object = (uint32_t)obj; a->prot = prot; a->flags = 0; a->reserved = 0;
    return syscall_dispatch(SYS_VM_MAP, MT_ARGS, 0, 0, 0, 0);
}
/* A held process running user_blob_touch in `mode` on byte 8 of page 0 of
   `m`, mapped for it with `prot` at the window's first page (*addr): its
   handle. The image is copied when the process is made, so the address is
   fixed before, by a hint its new, empty account always honours. */
static int32_t mt_toucher(struct kmem *m, uint32_t prot, uint64_t mode, int *tid, uint64_t *addr) {
    uint32_t len = (uint32_t)(user_blob_touch_end - user_blob_touch_start);
    if (len > KT_BLOB_MAX) return E_INTERNAL;
    uint64_t q[3] = { VM_MMAP_BASE + 8u, MT_VALUE, mode };
    for (uint32_t i = 0; i < len; i++) mt_image[i] = user_blob_touch_start[i];
    for (uint32_t i = 0; i < sizeof q; i++) mt_image[len + i] = ((const uint8_t *)q)[i];
    int32_t h = pt_make_in(mt_image, mt_image + len + sizeof q, tid, 0);
    if (h <= 0) return h;
    int vpid = thread_pid(*tid);
    vm_proc_init(vpid, vm_default_quota(), -1);
    struct vm_map_args a = { .length = PAGE_SIZE, .prot = prot, .flags = MMAP_HINT, .addr = VM_MMAP_BASE };
    if (vm_map_object(vpid, &a, m, HRIGHT_MAP | HRIGHT_MAP_WRITE) != E_OK || a.addr != VM_MMAP_BASE) {
        thread_discard(*tid);
        *tid = -1;
        htab_close(thread_htab(thread_current_id()), h);
        return E_INTERNAL;
    }
    *addr = a.addr;
    return h;
}

int usermode_mem_test(void) {
    int pid = thread_current_pid();
    if (vm_quota_of(pid)) return 0;      /* an account here would be clobbered */
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    vm_proc_init(pid, vm_default_quota(), -1);
    struct htab *tab = thread_htab(thread_current_id());
    struct vm_map_args *a = (struct vm_map_args *)user_ptr(MT_ARGS);
    const struct object_basic *ob = (const struct object_basic *)user_ptr(MT_INFO);
    const struct mem_info *mi = (const struct mem_info *)user_ptr(MT_INFO);
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    const uint32_t P = PAGE_SIZE;
    const uint32_t all = HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_MAP_EXEC | HRIGHT_SEAL |
                         HRIGHT_INSPECT | HRIGHT_DUPLICATE;
    uint32_t live0 = kobj_live(KOBJ_MEM), res0 = pmm_reserved_count();
    uint32_t faults0 = usermode_faults();
    int32_t m = -1, d = -1, t = -1, h = -1, s = -1, sd = -1;
    struct kmem *mo = 0;
    struct job *j = 0;
    uint64_t addr = 0, addr2 = 0, val = MT_VALUE, got = 0;
    paddr_t page0 = 0;
    uint32_t nmaps = 0;
    int tid = -1, step = 0, ok = 0, rc;

    /* 1: refusals make nothing; then an object of 3 pages less a bit, with
       the rights it is made with */
    step = 1; if (mt_create(0, 0) != E_INVALID_ARGS || mt_create(P, 1) != E_INVALID_ARGS
                  || mt_create(MMAP_MAX_BYTES + 1ull, 0) != E_OUT_OF_RANGE
                  || mt_create((1ull << 32) | P, 0) != E_OUT_OF_RANGE
                  || kobj_live(KOBJ_MEM) != live0 || pmm_reserved_count() != res0) goto done;
    m = mt_create(3u * P - 100u, 0);
    step = 2; if (m <= 0 || mt_info(m, OBJINFO_BASIC, sizeof *ob) != (int)sizeof *ob
                  || ob->kind != OBJ_TYPE_MEM || ob->rights != all
                  || kobj_live(KOBJ_MEM) != live0 + 1) goto done;

    /* 3: committed whole and nothing resident: three pages reserved */
    step = 3; if (mt_info(m, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi
                  || mi->size != 3u * P || mi->resident != 0
                  || pmm_reserved_count() != res0 + 3) goto done;
    t = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)m, HRIGHT_MAP, 0, 0, 0);
    step = 4; if (t <= 0 || d <= 0 || mt_info(m, OBJINFO_MEM, sizeof *mi - 1) != E_BUFFER_TOO_SMALL
                  || mt_info(t, OBJINFO_MEM, sizeof *mi) != E_WRONG_TYPE
                  || mt_info(d, OBJINFO_MEM, sizeof *mi) != E_ACCESS_DENIED) goto done;

    /* 5-6: mapping refusals, each its own error: the handle, the offset,
       the end past the object's (at 64 bits), then each right */
    step = 5; if (mt_map(9999, 0, P, MPROT_READ) != E_BAD_HANDLE
                  || mt_map(t, 0, P, MPROT_READ) != E_WRONG_TYPE
                  || mt_map(m, P + 1, P, MPROT_READ) != E_INVALID_ARGS
                  || mt_map(m, P, 3u * P, MPROT_READ) != E_OUT_OF_RANGE
                  || mt_map(m, 3u * P, 1, MPROT_READ) != E_OUT_OF_RANGE
                  || mt_map(m, 1ull << 63, P, MPROT_READ) != E_OUT_OF_RANGE
                  || mt_map(m, 0, 4u * P, MPROT_READ) != E_OUT_OF_RANGE) goto done;
    step = 6; if (mt_map(d, 0, P, MPROT_READ | MPROT_WRITE) != E_ACCESS_DENIED
                  || mt_map(d, 0, P, MPROT_READ | MPROT_EXEC) != E_ACCESS_DENIED
                  || mt_map(d, 0, P, MPROT_READ) != E_OK
                  || syscall_dispatch(SYS_VM_UNMAP, a->addr, 0, 0, 0, 0) != E_OK) goto done;
    htab_close(tab, d);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)m, HRIGHT_INSPECT | HRIGHT_MAP_WRITE, 0, 0, 0);
    step = 7; if (d <= 0 || mt_map(d, 0, P, MPROT_READ | MPROT_WRITE) != E_ACCESS_DENIED) goto done;
    htab_close(tab, d); d = -1;

    /* 8-9: a view of pages 1-2: nothing mapped until touched; a copy into
       page 2 touches it, through the check in front of the copy, and only it */
    step = 8; if (mt_map(m, P, 2u * P, MPROT_READ | MPROT_WRITE) != E_OK || a->granted != 2u * P) goto done;
    addr = a->addr;
    if (paging_get_phys((uintptr_t)addr) || paging_get_phys((uintptr_t)(addr + P))) goto done;
    mo = (struct kmem *)htab_ref(tab, m, KOBJ_MEM, 0, 0);
    step = 9; if (!mo || user_copy_out(addr + P + 8, &val, 8) != 0 || kmem_resident(mo) != 1
                  || !kmem_page_if_resident(mo, 2) || kmem_page_if_resident(mo, 1)
                  || paging_get_phys((uintptr_t)(addr + P)) != kmem_page_if_resident(mo, 2)
                  || paging_get_phys((uintptr_t)addr) != 0
                  || pmm_reserved_count() != res0 + 2
                  || *(volatile uint64_t *)((uint8_t *)paging_temp_map(kmem_page_if_resident(mo, 2)) + 8) != MT_VALUE)
        goto done;
    step = 10; if (mt_info(m, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi || mi->resident != P) goto done;

    /* 11: a second, read-only view of the whole object sees the same page;
       a write check on it touches nothing */
    step = 11; if (mt_map(m, 0, 3u * P, MPROT_READ) != E_OK) goto done;
    addr2 = a->addr;
    if (user_copy_in(&got, addr2 + 2u * P + 8, 8) != 0 || got != MT_VALUE
        || user_ptr_writable(addr2, 1) || paging_get_phys((uintptr_t)addr2) != 0
        || kmem_resident(mo) != 1) goto done;

    /* 12: unmapping takes the page out of the view, not out of the object */
    step = 12; if (syscall_dispatch(SYS_VM_UNMAP, addr, 0, 0, 0, 0) != E_OK
                   || paging_get_phys((uintptr_t)(addr + P)) != 0 || kmem_resident(mo) != 1
                   || user_ptr_readable(addr + P, 1)) goto done;
    addr = 0;
    if (syscall_dispatch(SYS_VM_UNMAP, addr2, 0, 0, 0, 0) != E_OK) goto done;
    addr2 = 0;

    /* 25-29: seals (1.3e). Refusals first, each its own error; a seal
       against writing is refused while a writable view stands, and adds
       nothing; then it binds every handle, and MEM_SEAL_SEAL ends the adding */
    s = mt_create(P, 0);
    sd = s > 0 ? syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)s, all & ~HRIGHT_SEAL, 0, 0, 0) : -1;
    step = 25; if (s <= 0 || sd <= 0 || mt_seal(s, 0) != E_INVALID_ARGS || mt_seal(s, 4) != E_INVALID_ARGS
                   || mt_seal(9999, MEM_SEAL_WRITE) != E_BAD_HANDLE
                   || mt_seal(t, MEM_SEAL_WRITE) != E_WRONG_TYPE
                   || mt_seal(sd, MEM_SEAL_WRITE) != E_ACCESS_DENIED
                   || mt_info(s, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi || mi->seals != 0) goto done;
    step = 26; if (mt_map(s, 0, P, MPROT_READ | MPROT_WRITE) != E_OK) goto done;
    addr = a->addr;
    if (mt_map(s, 0, P, MPROT_READ) != E_OK) goto done;
    addr2 = a->addr;
    if (mt_seal(s, MEM_SEAL_WRITE) != E_BUSY || mt_seal(s, MEM_SEAL_WRITE | MEM_SEAL_SEAL) != E_BUSY
        || mt_info(s, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi || mi->seals != 0) goto done;
    step = 27; if (syscall_dispatch(SYS_VM_UNMAP, addr, 0, 0, 0, 0) != E_OK) goto done;
    addr = 0;
    if (mt_seal(s, MEM_SEAL_WRITE) != E_OK
        || mt_info(s, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi || mi->seals != MEM_SEAL_WRITE) goto done;
    /* 28: no writable view through any handle; reading and running stay */
    step = 28; if (mt_map(s, 0, P, MPROT_READ | MPROT_WRITE) != E_ACCESS_DENIED
                   || mt_map(sd, 0, P, MPROT_READ | MPROT_WRITE) != E_ACCESS_DENIED
                   || mt_map(s, 0, P, MPROT_READ | MPROT_EXEC) != E_OK
                   || syscall_dispatch(SYS_VM_UNMAP, a->addr, 0, 0, 0, 0) != E_OK) goto done;
    step = 29; if (mt_seal(s, MEM_SEAL_WRITE) != E_OK || mt_seal(s, MEM_SEAL_SEAL) != E_OK
                   || mt_seal(s, MEM_SEAL_WRITE) != E_ACCESS_DENIED
                   || mt_info(s, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi
                   || mi->seals != (MEM_SEAL_WRITE | MEM_SEAL_SEAL)) goto done;
    if (syscall_dispatch(SYS_VM_UNMAP, addr2, 0, 0, 0, 0) != E_OK) goto done;
    addr2 = 0;
    htab_close(tab, sd); sd = -1;
    htab_close(tab, s); s = -1;

    /* 13: commit past a job's limit is refused at once and charges nothing */
    j = job_create(job_root(), 2u * P, 0, 0, &rc);
    step = 13; if (!j) goto done;
    struct kmem *k = kmem_create(3, j, &rc);
    if (k || rc != E_NO_MEMORY || j->mem_used != 0 || pmm_reserved_count() != res0 + 2) goto done;
    k = kmem_create(2, j, &rc);
    if (!k || j->mem_used != 2u * P || pmm_reserved_count() != res0 + 4) goto done;
    kobj_unref(&k->hdr);
    step = 14; if (j->mem_used != 0 || pmm_reserved_count() != res0 + 2) goto done;

    /* 15: and past what memory can promise: the margin stays, and once the
       rest is promised an allocation that would take a promised page is
       refused, and so is a new object. Interrupts off: nothing else may
       allocate while the pages are spoken for. */
    {
        paddr_t run = pmm_alloc_pages(512);
        step = 15; if (!run) goto done;
        pmm_free_pages(run, 512);
        uint32_t f = arch_irq_save();
        uint32_t avail = pmm_free_count() - PMM_COMMIT_MARGIN - pmm_reserved_count();
        int over = pmm_reserve(avail + 1), fit = -1;
        paddr_t taken = 0;
        int made = 0;
        if (over != 0) {
            fit = pmm_reserve(avail);
            if (fit == 0) {
                taken = pmm_alloc_pages(512);
                made = mt_create(P, 0);
                pmm_unreserve(avail);
            }
        } else {
            pmm_unreserve(avail + 1);   /* wrongly granted: give it back, then fail */
        }
        arch_irq_restore(f);
        if (taken) pmm_free_pages(taken, 512);
        if (made > 0) htab_close(tab, made);
        step = 16; if (over != -1 || fit != 0 || taken || made != E_NO_MEMORY
                       || pmm_reserved_count() != res0 + 2) goto done;
    }

    /* 17-18: from ring 3: the first touch of page 0 is a read, and finds it
       zero; the store holds; the page outlives the process, in the object */
    h = mt_toucher(mo, MPROT_READ | MPROT_WRITE, 0, &tid, &addr);
    step = 17; if (h <= 0 || mo->writers != 1) goto done;
    thread_start(tid); tid = -1;
    step = 18; if (pt_wait(h, SIGNAL_PROCESS_ENDED, PT_FOREVER, 0) != E_OK || pt_info(h) != (int)sizeof *pi
                   || pi->ended != PROC_EXITED || pi->code != 0) goto done;
    htab_close(tab, h); h = -1;
    page0 = kmem_page_if_resident(mo, 0);
    step = 19; if (!page0 || kmem_resident(mo) != 2|| mo->writers != 0 || !pmm_page_in_use(page0)
                   || *(volatile uint64_t *)((uint8_t *)paging_temp_map(page0) + 8) != MT_VALUE) goto done;

    /* 20: a store as the first touch of a read-only view is the process's
       fault, at that address, and maps nothing */
    h = mt_toucher(mo, MPROT_READ, 1, &tid, &addr);
    step = 20; if (h <= 0) goto done;
    thread_start(tid); tid = -1;
    step = 21; if (pt_wait(h, SIGNAL_PROCESS_ENDED, PT_FOREVER, 0) != E_OK || pt_info(h) != (int)sizeof *pi
                   || pi->ended != PROC_CRASHED || pi->vector != 14 || pi->addr != addr + 8
                   || usermode_faults() != faults0 + 1 || kmem_resident(mo) != 2) goto done;
    htab_close(tab, h); h = -1;
    addr = 0;

    /* 22: the last handle frees the object, its pages and what it still had
       reserved */
    kobj_unref(&mo->hdr); mo = 0;
    htab_close(tab, m); m = -1;
    step = 22; if (kobj_live(KOBJ_MEM) != live0 || pmm_page_in_use(page0)
                   || pmm_reserved_count() != res0) goto done;

    /* 23: as many regions as a process may hold, and not one more */
    struct vm_map_args v = { .length = 1, .prot = MPROT_READ };
    step = 23;
    for (nmaps = 0; nmaps < MMAP_MAX_REGIONS; nmaps++) {
        v.addr = 0;
        if (vm_map(pid, &v) != E_OK) goto done;
        mt_addrs[nmaps] = v.addr;
    }
    v.addr = 0;
    step = 24; if (vm_map(pid, &v) != E_NO_RESOURCES) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "memory object test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    while (nmaps) vm_unmap(pid, mt_addrs[--nmaps], 0);
    if (tid >= 0) thread_discard(tid);
    if (h > 0) htab_close(tab, h);
    if (addr) vm_unmap(pid, addr, 0);
    if (addr2) vm_unmap(pid, addr2, 0);
    if (mo) kobj_unref(&mo->hdr);
    if (j) job_put(j);
    if (m > 0) htab_close(tab, m);
    if (sd > 0) htab_close(tab, sd);
    if (s > 0) htab_close(tab, s);
    if (d > 0) htab_close(tab, d);
    if (t > 0) htab_close(tab, t);
    vm_proc_reset(pid);
    unmap_user_page(TEST_CODE_VIRT, phys);
    if (ok && kobj_live(KOBJ_MEM) != live0) ok = 0;
    return ok;
}

/* ---- IPC (Phase 1.3a) ----
 * The test thread is the server, on an endpoint of its own; callers are
 * ring-3 processes running user_blob_calls: two yields, then SYS_IPC_CALL
 * with their own arguments, exiting with what it returned. Minting and its
 * refusals; calls queued oldest first with their badges; a message that does
 * not fit stays; replies in any order, one each; a reply too long or unreadable
 * keeps its token; a token closed unanswered, and an endpoint whose server
 * has gone, fail their callers; a call's bytes are charged to its caller's
 * job; a queue wakes for a call; nothing is left behind. 1 = pass. */
#define IT_ARGS(i)  (TEST_CODE_VIRT + 0xA00 + (i) * 0x40)   /* caller i's ipc_call_args */
#define IT_REQ(i)   (TEST_CODE_VIRT + 0xB00 + (i) * 0x10)   /* its request */
#define IT_REPLY(i) (TEST_CODE_VIRT + 0xC00 + (i) * 0x40)   /* its reply buffer */
#define IT_RCAP     8                                       /* the reply it takes */
#define IT_BIG      (TEST_CODE_VIRT + 0x400)                /* a reply buffer of 0x400 */
#define IT_TARGS    (TEST_CODE_VIRT + 0xE00)   /* the test thread's recv and reply args */
#define IT_TINFO    (TEST_CODE_VIRT + 0xE40)   /* its ipc_msg_info */
#define IT_TBUF     (TEST_CODE_VIRT + 0xE80)   /* its receive buffer, 64 bytes */
#define IT_DATA     (TEST_CODE_VIRT + 0xF00)   /* its reply bytes */

/* A caller, `i` of the slots, in `j` (NULL: this thread's), calling through
   `send` if given (an ep_send, installed as it is) or else a new send handle
   on `ep` with `badge`: `req_len` bytes of IT_REQ(i), a reply of up to `rcap`
   into `reply`. Started at once if `start`. The process handle, or <= 0. */
static int32_t it_caller(struct endpoint *ep, struct kobj *send, uint64_t badge, uint32_t i,
                         uint64_t req_len, uint64_t reply, uint64_t rcap,
                         struct job *j, int start, int *tid) {
    st_len = (uint32_t)(user_blob_calls_end - user_blob_calls_start);
    if (st_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t k = 0; k < st_len; k++) st_image[k] = user_blob_calls_start[k];
    int32_t h = pt_make_in(st_image, st_image + st_len + 3 * 6 * 8, tid, j);
    if (h <= 0) return h;
    int32_t v = send ? htab_install(thread_htab(*tid), send, HRIGHT_SEND, 0)
                     : ep_install_send(*tid, ep, badge, HRIGHT_SEND, 0);
    if (v <= 0) {
        thread_discard(*tid);
        htab_close(thread_htab(thread_current_id()), h);
        return E_NO_RESOURCES;
    }
    struct ipc_call_args *a = (struct ipc_call_args *)user_ptr(IT_ARGS(i));
    a->req_len = req_len; a->reply_cap = rcap; a->req = IT_REQ(i); a->reply = reply;
    a->ep_handle = v; a->reserved = 0;
    a->handles = 0; a->handle_count = 0; a->reply_handles = 0; a->reply_handle_cap = 0;
    st_call(0, SYS_YIELD, 0, 0);
    st_call(1, SYS_YIELD, 0, 0);
    st_call(2, SYS_IPC_CALL, IT_ARGS(i), 0);
    if (start) thread_start(*tid);
    return h;
}

/* `n` calls are queued on `ep`, within a second. */
static int it_queued(struct endpoint *ep, uint32_t n) {
    for (int k = 0; k < 100; k++) {
        if (ep_queued(ep) == n) return 1;
        (void)thread_sleep_ms(10);
    }
    return 0;
}

/* `h` exits with `code`, the result of its call. */
static int it_ends(int32_t h, int32_t code) {
    struct process_info *pi = (struct process_info *)user_ptr(PT_INFO);
    return pt_wait(h, SIGNAL_PROCESS_ENDED, 3000, 0) == E_OK && pt_info(h) == (int)sizeof *pi
        && pi->ended == PROC_EXITED && pi->code == code;
}

static int it_recv(int32_t eh, uint64_t cap) {
    struct ipc_recv_args *r = (struct ipc_recv_args *)user_ptr(IT_TARGS);
    r->cap = cap; r->buf = IT_TBUF; r->info = IT_TINFO; r->ep_handle = eh; r->handles = 0; r->handle_cap = 0;
    return syscall_dispatch(SYS_IPC_RECV, IT_TARGS, 0, 0, 0, 0);
}

static int it_reply(int32_t tok, uint64_t data, uint64_t len) {
    struct ipc_reply_args *r = (struct ipc_reply_args *)user_ptr(IT_TARGS);
    r->data = data; r->len = len; r->token = tok; r->handles = 0; r->handle_count = 0;
    return syscall_dispatch(SYS_IPC_REPLY, IT_TARGS, 0, 0, 0, 0);
}

static int it_mint(int32_t h, uint64_t badge, uint32_t rights) {
    return um_call(SYS_EP_MINT, 1u << 1, (uint32_t)h, badge, rights, 0, 0);
}

/* SYS_OBJECT_WAIT only looks: is a call or a notice waiting? */
static int it_readable(int32_t eh) {
    return syscall_dispatch(SYS_OBJECT_WAIT, (uint32_t)eh, SIGNAL_EP_READABLE, 0, 0, 0) == E_OK;
}

/* OBJINFO_BASIC on `h` says `kind` and `rights`. */
static int it_basic(int32_t h, uint32_t kind, uint32_t rights) {
    const struct object_basic *ob = (const struct object_basic *)user_ptr(JT_INFO);
    return jt_info_as(h, OBJINFO_BASIC, sizeof *ob) == (int)sizeof *ob
        && ob->kind == kind && ob->rights == rights;
}

int usermode_ipc_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    const struct ipc_msg_info *ti = (const struct ipc_msg_info *)user_ptr(IT_TINFO);
    const uint8_t *tbuf = (const uint8_t *)user_ptr(IT_TBUF);
    volatile uint8_t *data = (volatile uint8_t *)user_ptr(IT_DATA);
    volatile struct queue_event *ev = (volatile struct queue_event *)user_ptr(QT_EV);
    uint32_t live0 = proc_live(), faults0 = usermode_faults(), refused0 = syscall_refused_count();
    uint32_t sends0 = kobj_live(KOBJ_SEND), calls0 = kobj_live(KOBJ_REPLY), jobs0 = job_live();
    int eps0 = ep_in_use_count();
    struct endpoint *ep = 0, *ep2 = 0;
    struct kobj *s7o = 0;
    int32_t eh = -1, e2 = -1, s7 = -1, d = -1, t = -1, q = -1, j = -1, tok = -1, tokb = -1;
    int32_t ha = -1, hb = -1, hc = -1, hd = -1, he = -1;
    int tida = -1, tidb = -1, tidc = -1, tidd = -1, tide = -1, step = 0, ok = 0;
    struct job *jb;
    struct ipc_call_args *ca = (struct ipc_call_args *)user_ptr(IT_ARGS(5));

    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;

    /* 1: minting: the refusals, then a badged send handle with the rights
       asked for, and nothing waiting on the endpoint yet */
    step = 1; if (!ep || !it_basic(eh, OBJ_TYPE_ENDPOINT, HRIGHT_RECV | HRIGHT_WAIT | HRIGHT_DUPLICATE)
                  || it_readable(eh)) goto done;
    t = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    d = syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)eh, HRIGHT_WAIT, 0, 0, 0);
    if (t <= 0 || d <= 0
        || it_mint(9999, 7, HRIGHT_SEND) != E_BAD_HANDLE
        || it_mint(t, 7, HRIGHT_SEND) != E_WRONG_TYPE
        || it_mint(d, 7, HRIGHT_SEND) != E_ACCESS_DENIED
        || it_mint(eh, 7, 0) != E_ACCESS_DENIED
        || it_mint(eh, 7, HRIGHT_DUPLICATE) != E_ACCESS_DENIED
        || it_mint(eh, 7, HRIGHT_SEND | HRIGHT_RECV) != E_ACCESS_DENIED
        || it_mint(eh, 7, HRIGHT_SEND | HRIGHT_WAIT) != E_ACCESS_DENIED
        || kobj_live(KOBJ_SEND) != sends0) goto done;
    s7 = it_mint(eh, 0x7777000000000007ull, HRIGHT_SEND | HRIGHT_DUPLICATE);
    s7o = s7 > 0 ? htab_ref(tab, s7, KOBJ_SEND, 0, 0) : 0;
    if (!s7o || ((struct ep_send *)s7o)->badge != 0x7777000000000007ull
        || !it_basic(s7, OBJ_TYPE_SEND, HRIGHT_SEND | HRIGHT_DUPLICATE)
        || it_mint(s7, 7, HRIGHT_SEND) != E_WRONG_TYPE
        || kobj_live(KOBJ_SEND) != sends0 + 1) goto done;

    /* 2: refusals before anything queues: too long, and each handle the
       wrong kind for its call */
    ca->req_len = IPC_MSG_MAX + 1; ca->reply_cap = 0; ca->req = IT_REQ(5); ca->reply = 0;
    ca->ep_handle = s7; ca->reserved = 0;
    ca->handles = 0; ca->handle_count = 0; ca->reply_handles = 0; ca->reply_handle_cap = 0;
    step = 2; if (syscall_dispatch(SYS_IPC_CALL, IT_ARGS(5), 0, 0, 0, 0) != E_OUT_OF_RANGE) goto done;
    ca->req_len = 0; ca->ep_handle = eh;
    if (syscall_dispatch(SYS_IPC_CALL, IT_ARGS(5), 0, 0, 0, 0) != E_BAD_HANDLE
        || it_recv(s7, 64) != E_BAD_HANDLE || it_reply(eh, IT_DATA, 0) != E_BAD_HANDLE
        || it_reply(s7, IT_DATA, 0) != E_BAD_HANDLE) goto done;
    {
        struct ipc_recv_args *r = (struct ipc_recv_args *)user_ptr(IT_TARGS);
        r->cap = 64; r->buf = IT_TBUF; r->info = 0; r->ep_handle = eh; r->handles = 0; r->handle_cap = 0;
        if (syscall_dispatch(SYS_IPC_RECV, IT_TARGS, 0, 0, 0, 0) != E_BAD_ADDRESS) goto done;
    }
    if (ep_queued(ep) != 0 || kobj_live(KOBJ_REPLY) != calls0) goto done;

    /* 3: two callers, queued in the order they called: A through the minted
       handle, B with badge 9. The endpoint is readable. */
    *(volatile uint8_t *)user_ptr(IT_REQ(0)) = 'A';
    ((volatile uint8_t *)user_ptr(IT_REQ(1)))[0] = 'B';
    ((volatile uint8_t *)user_ptr(IT_REQ(1)))[1] = 'b';
    ha = it_caller(ep, s7o, 0, 0, 1, IT_REPLY(0), IT_RCAP, 0, 1, &tida);
    step = 3; if (ha <= 0 || !it_queued(ep, 1) || !it_readable(eh)) goto done;
    hb = it_caller(ep, 0, 9, 1, 2, IT_REPLY(1), IT_RCAP, 0, 1, &tidb);
    if (hb <= 0 || !it_queued(ep, 2) || kobj_live(KOBJ_REPLY) != calls0 + 2
        || !thread_is_blocked(tida) || !thread_is_blocked(tidb)) goto done;

    /* 4: no room for A's byte: told its length, given no token, and A
       stays first */
    step = 4; if (it_recv(eh, 0) != E_BUFFER_TOO_SMALL || ti->len != 1 || ti->reply != 0
                  || ti->badge != 0x7777000000000007ull || ep_queued(ep) != 2) goto done;

    /* 5-6: received in order, each with its badge, its caller, the reply
       it takes and a token of its own that cannot be copied; then nothing
       is waiting */
    step = 5; if (it_recv(eh, 64) != 1 || tbuf[0] != 'A' || ti->badge != 0x7777000000000007ull
                  || ti->sender != thread_pid(tida) || ti->len != 1 || ti->reply_cap != IT_RCAP
                  || ti->reply <= 0 || ep_queued(ep) != 1 || !it_readable(eh)) goto done;
    tok = ti->reply;
    if (!it_basic(tok, OBJ_TYPE_REPLY, HRIGHT_REPLY)
        || syscall_dispatch(SYS_HANDLE_DUPLICATE, (uint32_t)tok, HRIGHT_REPLY, 0, 0, 0) != E_ACCESS_DENIED) goto done;
    step = 6; if (it_recv(eh, 64) != 2 || tbuf[0] != 'B' || tbuf[1] != 'b' || ti->badge != 9
                  || ti->sender != thread_pid(tidb) || ti->reply <= 0 || ti->reply == tok
                  || ep_queued(ep) != 0 || it_readable(eh)) goto done;
    tokb = ti->reply;

    /* 7: B answered first, while A waits; its token is spent */
    data[0] = 'x'; data[1] = 'y'; data[2] = 'z';
    step = 7; if (it_reply(tokb, IT_DATA, 3) != E_OK || !it_ends(hb, 3)) goto done;
    {
        const uint8_t *rb = (const uint8_t *)user_ptr(IT_REPLY(1));
        if (rb[0] != 'x' || rb[1] != 'y' || rb[2] != 'z') goto done;
    }
    if (it_reply(tokb, IT_DATA, 3) != E_BAD_HANDLE || !thread_is_blocked(tida)) goto done;
    tokb = -1;

    /* 8: A's reply: too long for it, then unreadable, each keeping the
       token; then one that fits */
    step = 8; if (it_reply(tok, IT_DATA, IT_RCAP + 1) != E_BUFFER_TOO_SMALL
                  || it_reply(tok, USER_VLIMIT, 2) != E_BAD_ADDRESS
                  || !thread_is_blocked(tida) || kobj_live(KOBJ_REPLY) != calls0 + 1) goto done;
    data[0] = 'o'; data[1] = 'k';
    if (it_reply(tok, IT_DATA, 2) != E_OK || !it_ends(ha, 2)
        || ((const uint8_t *)user_ptr(IT_REPLY(0)))[1] != 'k') goto done;
    tok = -1;
    if (kobj_live(KOBJ_REPLY) != calls0) goto done;

    /* 9: a token closed unanswered fails its caller */
    hc = it_caller(ep, 0, 0, 2, 0, IT_REPLY(2), IT_RCAP, 0, 1, &tidc);
    step = 9; if (hc <= 0 || !it_queued(ep, 1) || it_recv(eh, 64) != 0 || ti->reply <= 0
                  || ti->badge != 0) goto done;
    if (htab_close(tab, ti->reply) != E_OK || !it_ends(hc, E_PEER_CLOSED)
        || kobj_live(KOBJ_REPLY) != calls0) goto done;
    htab_close(tab, hc); hc = -1;

    /* 10: the server goes: a queued call fails, a later one is refused,
       and the send handles keep only the slot */
    e2 = ep_create();
    ep2 = e2 > 0 ? ep_from_handle(e2, HRIGHT_RECV, 0) : 0;
    step = 10; if (!ep2) goto done;
    hd = it_caller(ep2, 0, 0, 3, 0, IT_REPLY(3), IT_RCAP, 0, 1, &tidd);
    if (hd <= 0 || !it_queued(ep2, 1)) goto done;
    he = it_caller(ep2, 0, 0, 4, 0, IT_REPLY(4), IT_RCAP, 0, 0, &tide);
    if (he <= 0) goto done;
    htab_close(tab, e2); e2 = -1;
    if (ep2->active || ep_queued(ep2) != 0 || !it_ends(hd, E_PEER_CLOSED)) goto done;
    thread_start(tide);
    if (!it_ends(he, E_PEER_CLOSED) || kobj_live(KOBJ_REPLY) != calls0) goto done;
    ep_put(ep2); ep2 = 0;

    /* 11: a call's bytes are charged to its caller's job while it lasts,
       and one past the job's budget is refused */
    j = jt_create(0, 0x100, 0, 0);
    jb = j > 0 ? jt_job(j) : 0;
    step = 11; if (!jb || jb->mem_used != 0) goto done;
    hc = it_caller(ep, 0, 0, 2, 1, IT_BIG, 0x200, jb, 1, &tidc);
    if (hc <= 0 || !it_ends(hc, E_NO_MEMORY) || jb->mem_used != 0 || ep_queued(ep) != 0) goto done;
    htab_close(tab, hc); hc = -1;
    hc = it_caller(ep, 0, 0, 2, 1, IT_BIG, 0x80, jb, 1, &tidc);
    if (hc <= 0 || !it_queued(ep, 1) || jb->mem_used != 0x80
        || it_recv(eh, 64) != 1 || ti->reply_cap != 0x80) goto done;
    if (it_reply(ti->reply, IT_DATA, 0) != E_OK || !it_ends(hc, 0) || jb->mem_used != 0) goto done;
    htab_close(tab, hc); hc = -1;

    /* 12: a queue registered for the endpoint is woken by a call */
    q = syscall_dispatch(SYS_QUEUE_CREATE, 0, 0, 0, 0, 0);
    step = 12; if (q <= 0 || qt_reg(q, eh, SIGNAL_EP_READABLE, 0x1234) != E_OK) goto done;
    hc = it_caller(ep, 0, 0, 2, 0, IT_REPLY(2), IT_RCAP, 0, 1, &tidc);
    if (hc <= 0 || qt_wait(q, 1, timer_now_ns() + 2000ull * TIMER_NS_PER_TICK) != 1
        || ev[0].key != 0x1234 || !(ev[0].signals & SIGNAL_EP_READABLE)
        || it_recv(eh, 64) != 0 || it_reply(ti->reply, IT_DATA, 0) != E_OK || !it_ends(hc, 0)) goto done;
    htab_close(tab, q); q = -1;

    /* 13: nothing left behind, and nothing ended any other way */
    htab_close(tab, ha); ha = -1;
    htab_close(tab, hb); hb = -1;
    htab_close(tab, hc); hc = -1;
    htab_close(tab, hd); hd = -1;
    htab_close(tab, he); he = -1;
    htab_close(tab, j); j = -1;
    kobj_unref(s7o); s7o = 0;
    htab_close(tab, s7); s7 = -1;
    step = 13; if (proc_live() != live0 || usermode_faults() != faults0
                   || syscall_refused_count() != refused0 || kobj_live(KOBJ_SEND) != sends0
                   || kobj_live(KOBJ_REPLY) != calls0 || ep_in_use_count() != eps0 + 1
                   || job_live() != jobs0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "ipc test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* A caller still there is killed, so none outlives the test. */
    {
        int32_t hs[5] = { ha, hb, hc, hd, he };
        for (int k = 0; k < 5; k++) {
            if (hs[k] <= 0) continue;
            kt_kill_call(hs[k]);
            pt_wait(hs[k], SIGNAL_PROCESS_ENDED, 3000, 0);
            htab_close(tab, hs[k]);
        }
    }
    (void)tidd; (void)tide;
    if (tok > 0)  htab_close(tab, tok);
    if (tokb > 0) htab_close(tab, tokb);
    if (s7o) kobj_unref(s7o);
    if (s7 > 0) htab_close(tab, s7);
    if (d > 0)  htab_close(tab, d);
    if (t > 0)  htab_close(tab, t);
    if (q > 0)  htab_close(tab, q);
    if (j > 0)  htab_close(tab, j);
    if (ep2) ep_put(ep2);
    if (e2 > 0) htab_close(tab, e2);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- handles in messages (Phase 1.3b) ----
 * Callers are ring-3 processes as in the IPC test, each given handles on this
 * thread's timers before it starts. Every refusal is a caller's call result,
 * and a refused call leaves its handles where they were; a request moves one
 * handle with fewer rights and copies another, and a receive with no room for
 * them leaves both in transit; the reply's refusals keep its token and its
 * handles; a reply moves one back with its grant intact; a call that fails or
 * is withdrawn closes what it carried; a sealed memory object travels and its
 * receiver sees the seals (1.3e). 1 = pass. */
#define HT_DESC     (TEST_CODE_VIRT + 0x300)               /* this thread's ipc_handle[8] */
#define HT_CDESC(i) (TEST_CODE_VIRT + 0x380 + (i) * 0x20)  /* caller i's, two each */
#define HT_RH(i)    (TEST_CODE_VIRT + 0x900 + (i) * 0x40)  /* caller i's reply handles */
#define HT_TH       (TEST_CODE_VIRT + 0xEC0)               /* the handles this thread receives */
#define HT_RHCAP    3

static void ht_desc(uint64_t at, uint32_t k, int32_t h, uint32_t op, uint32_t rights) {
    struct ipc_handle *d = (struct ipc_handle *)user_ptr(at) + k;
    d->handle = h; d->op = op; d->rights = rights; d->reserved = 0;
}

/* Caller `i` on `ep` (not started), holding a handle with `rights[k]` on
   each of `objs[0..n)`, their values in hv[]. Its call has no bytes, takes
   HT_RHCAP handles back into HT_RH(i), and sends HT_CDESC(i), for ht_desc to
   fill and ht_go to count. The process handle, or <= 0. */
static int32_t ht_caller(struct endpoint *ep, uint32_t i, struct kobj **objs, const uint32_t *rights,
                         uint32_t n, int32_t *hv, int *tid) {
    int32_t h = it_caller(ep, 0, 0, i, 0, IT_REPLY(i), IT_RCAP, 0, 0, tid);
    if (h <= 0) return h;
    for (uint32_t k = 0; k < n; k++) {
        hv[k] = htab_install(thread_htab(*tid), objs[k], rights[k], 0);
        if (hv[k] <= 0) {
            thread_discard(*tid);
            htab_close(thread_htab(thread_current_id()), h);
            return E_NO_RESOURCES;
        }
    }
    struct ipc_call_args *a = (struct ipc_call_args *)user_ptr(IT_ARGS(i));
    a->handles = HT_CDESC(i); a->reply_handles = HT_RH(i); a->reply_handle_cap = HT_RHCAP;
    return h;
}

static void ht_go(int tid, uint32_t i, uint32_t ndesc) {
    ((struct ipc_call_args *)user_ptr(IT_ARGS(i)))->handle_count = ndesc;
    thread_start(tid);
}

static int ht_recv(int32_t eh, uint32_t hcap) {
    struct ipc_recv_args *r = (struct ipc_recv_args *)user_ptr(IT_TARGS);
    r->cap = 64; r->buf = IT_TBUF; r->info = IT_TINFO; r->handles = HT_TH;
    r->ep_handle = eh; r->handle_cap = hcap;
    return syscall_dispatch(SYS_IPC_RECV, IT_TARGS, 0, 0, 0, 0);
}

static int ht_reply(int32_t tok, uint64_t len, uint32_t n) {
    struct ipc_reply_args *r = (struct ipc_reply_args *)user_ptr(IT_TARGS);
    r->data = IT_DATA; r->len = len; r->handles = HT_DESC; r->token = tok; r->handle_count = n;
    return syscall_dispatch(SYS_IPC_REPLY, IT_TARGS, 0, 0, 0, 0);
}

/* `h` in `t` names `o` with exactly `rights`; *grant and *parent, if given,
   get the handle's. */
static int ht_names(struct htab *t, int32_t h, struct kobj *o, uint32_t rights,
                    uint32_t *grant, uint32_t *parent) {
    struct htab_info hi;
    struct kobj *r = htab_ref(t, h, KOBJ_NONE, 0, &hi);
    if (!r) return 0;
    kobj_unref(r);
    if (grant)  *grant  = hi.grant;
    if (parent) *parent = hi.parent;
    return r == o && hi.rights == rights;
}

static uint32_t ht_handles(struct kobj *o) { return atomic_load(&o->handles); }

/* The receiver of a moved open file (1.3b'): a kernel thread in its own
   process, holding a receive handle on the endpoint. It takes the call,
   waits until the test thread is asleep on the file's lock, unlocks it
   through the handle it received, then closes that and replies with no
   bytes. Its waits are bounded, so a test thread that never sleeps (a false
   E_DEADLOCK) still gets its call answered. */
static volatile int    hf_go, hf_stage, hf_waited, hf_unlock;
static volatile int32_t hf_eh;

static void hf_helper(void) {
    while (!hf_go) yield();
    const struct ipc_msg_info *ti = (const struct ipc_msg_info *)user_ptr(IT_TINFO);
    const int32_t *th = (const int32_t *)user_ptr(HT_TH);
    if (ht_recv(hf_eh, 1) != 0 || ti->handles != 1 || ti->reply <= 0) { hf_stage = -1; return; }
    int32_t f = th[0], tok = ti->reply;
    hf_stage = 1;
    for (int i = 0; i < 200 && sysfile_lock_waiting() == 0; i++) yield();
    hf_waited = sysfile_lock_waiting();
    hf_unlock = syscall_dispatch(SYS_FILE_UNLOCK, (uint32_t)f, 0, 0, 0, 0);
    hf_stage = 2;
    syscall_dispatch(SYS_FILE_CLOSE, (uint32_t)f, 0, 0, 0, 0);
    ht_reply(tok, 0, 0);
    hf_stage = 3;
}

int usermode_ipc_handles_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    struct htab *tab = thread_htab(thread_current_id());
    const struct ipc_msg_info *ti = (const struct ipc_msg_info *)user_ptr(IT_TINFO);
    const int32_t *th = (const int32_t *)user_ptr(HT_TH);
    volatile int32_t *rh = (volatile int32_t *)user_ptr(HT_RH(0));
    volatile uint8_t *data = (volatile uint8_t *)user_ptr(IT_DATA);
    uint32_t live0 = proc_live(), faults0 = usermode_faults(), refused0 = syscall_refused_count();
    uint32_t calls0 = kobj_live(KOBJ_REPLY), timers0 = kobj_live(KOBJ_TIMER);
    uint32_t mems0 = kobj_live(KOBJ_MEM);
    struct kmem *mb = 0;
    int eps0 = ep_in_use_count();
    struct endpoint *ep = 0, *ep2 = 0;
    int files0 = sysfile_open_count();
    uint32_t caps0 = thread_current_caps();
    struct kobj *o1 = 0, *o2 = 0, *fo = 0;
    int32_t eh = -1, e2 = -1, t1 = -1, t2 = -1, tok = -1, ha = -1, hb = -1, th0 = -1, th1 = -1, fh = -1, fb = -1;
    int32_t hv[2];
    int hf_tid = -1;
    uint32_t g0 = 0, g1 = 0, gt2 = 0, g = 0, p = 0;
    int tida = -1, tidb = -1, step = 0, ok = 0;
    const uint32_t ALL = HRIGHT_WAIT | HRIGHT_ARM | HRIGHT_DUPLICATE;

    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    t1 = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    t2 = syscall_dispatch(SYS_TIMER_CREATE, 0, 0, 0, 0, 0);
    o1 = t1 > 0 ? htab_ref(tab, t1, KOBJ_TIMER, 0, 0) : 0;
    o2 = t2 > 0 ? htab_ref(tab, t2, KOBJ_TIMER, 0, 0) : 0;
    /* 1: an open file travels, its locks with it (sysfile.c); so do the IPC types */
    step = 1; if (!ep || !o1 || !o2 || !ht_names(tab, t2, o2, ALL, &gt2, 0)
                  || !kobj_type_ops(KOBJ_FILE)->transfer || !kobj_type_ops(KOBJ_ENDPOINT)->transfer
                  || !kobj_type_ops(KOBJ_SEND)->transfer || !kobj_type_ops(KOBJ_REPLY)->transfer) goto done;

    /* 2-8: refusals, each its caller's result, with nothing queued: a dead
       handle beside a good one, rights that grow, a copy without the
       duplicate right, a handle named twice, an unknown op, the endpoint the
       call goes to, and descriptors it cannot read */
    {
        struct kobj *objs[2] = { o1, o2 };
        const uint32_t rights[2] = { ALL, HRIGHT_WAIT | HRIGHT_ARM };
        static const struct { uint32_t n, op0, r0, op1; int32_t want; } rf[] = {
            { 2, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME, IPC_HANDLE_MOVE, E_BAD_HANDLE },     /* 9999 */
            { 1, IPC_HANDLE_MOVE, HRIGHT_RECV,     IPC_HANDLE_MOVE, E_ACCESS_DENIED },
            { 1, IPC_HANDLE_COPY, IPC_RIGHTS_SAME, IPC_HANDLE_MOVE, E_ACCESS_DENIED },  /* hv[1] */
            { 2, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME, IPC_HANDLE_COPY, E_INVALID_ARGS },   /* hv[0] twice */
            { 1, 3,               IPC_RIGHTS_SAME, IPC_HANDLE_MOVE, E_INVALID_ARGS },
        };
        for (uint32_t k = 0; k < sizeof rf / sizeof rf[0]; k++) {
            ha = ht_caller(ep, 0, objs, rights, 2, hv, &tida);
            step = 2 + (int)k; if (ha <= 0) goto done;
            ht_desc(HT_CDESC(0), 0, k == 2 ? hv[1] : hv[0], rf[k].op0, rf[k].r0);
            ht_desc(HT_CDESC(0), 1, k == 0 ? 9999 : hv[0], rf[k].op1, IPC_RIGHTS_SAME);
            ht_go(tida, 0, rf[k].n);
            if (!it_ends(ha, rf[k].want) || ep_queued(ep) != 0) goto done;
            htab_close(tab, ha); ha = -1;
        }
        int32_t rv = -1;
        ha = ht_caller(ep, 0, objs, rights, 0, hv, &tida);
        step = 7; if (ha <= 0 || (rv = ep_install_handle(tida, ep, HRIGHT_RECV, 0)) <= 0) goto done;
        ht_desc(HT_CDESC(0), 0, rv, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
        ht_go(tida, 0, 1);
        if (!it_ends(ha, E_NOT_SUPPORTED) || ep_queued(ep) != 0) goto done;
        htab_close(tab, ha); ha = -1;
        /* 71-76: an open file travels, and its locks go with it (1.3b').
           This thread locks the whole file, gives the caller the only other
           handle, closes its own and lets the caller move that one. The lock
           still stands; waiting for it through a second open is no
           E_DEADLOCK, though this thread took it, since who holds the file
           is now someone else; the receiver unlocks it and the wait ends. */
        if (cxfs_is_mounted()) {
            const char *scratch = (cxfs_resolve("/Temp", 0) >= 0) ? "/Temp" : "";
            struct htab_info fi;
            ft_strp(FT_PATH, scratch, "/kt_ht.txt");
            thread_set_caps(thread_current_id(), GRANT_DISK);
            fh = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0,
                         FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNC);
            fb = ft_call(SYS_FILE_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
            int lrc = fh >= 0 ? ft_call(SYS_FILE_LOCK, fh, 0, 0, 0, 0, FLOCK_EXCLUSIVE) : fh;
            thread_set_caps(thread_current_id(), caps0);
            fo = fh >= 0 ? htab_ref(tab, fh, KOBJ_FILE, 0, &fi) : 0;
            step = 71; if (!fo || fb < 0 || lrc != E_OK) goto done;
            struct kobj *fobjs[1] = { fo };
            ha = ht_caller(ep, 0, fobjs, &fi.rights, 1, hv, &tida);
            step = 72; if (ha <= 0) goto done;
            htab_close(tab, fh); fh = -1;
            ht_desc(HT_CDESC(0), 0, hv[0], IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
            ht_go(tida, 0, 1);
            step = 73; if (!it_queued(ep, 1) || ht_handles(fo) != 1) goto done;
            hf_go = 0; hf_stage = 0; hf_waited = 0; hf_unlock = 1;
            hf_tid = thread_create("kthf", hf_helper);
            step = 74; if (hf_tid < 0) goto done;
            thread_set_caps(hf_tid, GRANT_DISK);
            hf_eh = ep_install_handle(hf_tid, ep, HRIGHT_RECV, 0);
            if (hf_eh <= 0) goto done;
            hf_go = 1;
            thread_set_caps(thread_current_id(), GRANT_DISK);
            lrc = ft_call(SYS_FILE_LOCK, fb, 0, 0, 0, 0, FLOCK_EXCLUSIVE | FLOCK_WAIT);
            thread_set_caps(thread_current_id(), caps0);
            step = 75; if (lrc != E_OK) goto done;
            for (int i = 0; i < 200 && thread_is_alive(hf_tid); i++) yield();
            step = 76; if (hf_stage != 3 || hf_waited != 1 || hf_unlock != E_OK
                           || thread_is_alive(hf_tid) || !it_ends(ha, 0) || ep_queued(ep) != 0) goto done;
            hf_tid = -1;
            htab_close(tab, ha); ha = -1;
            kobj_unref(fo); fo = 0;
            htab_close(tab, fb); fb = -1;
            ft_strp(FT_PATH, scratch, "/kt_ht.txt");      /* it_ends wrote PT_INFO over it */
            thread_set_caps(thread_current_id(), GRANT_DISK);
            int rc = ft_call(SYS_FILE_UNLINK, 0, FT_PATH, 0, 0, 0, 0);
            thread_set_caps(thread_current_id(), caps0);
            if (rc != 0) goto done;
        }
        ha = ht_caller(ep, 0, objs, rights, 1, hv, &tida);
        step = 8; if (ha <= 0) goto done;
        ((struct ipc_call_args *)user_ptr(IT_ARGS(0)))->handles = USER_VLIMIT;
        ht_go(tida, 0, 1);
        if (!it_ends(ha, E_BAD_ADDRESS)) goto done;
        htab_close(tab, ha); ha = -1;

        /* 9: the first refusal again, then a close of the good handle: it
           is still where it was */
        ha = ht_caller(ep, 0, objs, rights, 2, hv, &tida);
        step = 9; if (ha <= 0) goto done;
        ht_desc(HT_CDESC(0), 0, hv[0], IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
        ht_desc(HT_CDESC(0), 1, 9999, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
        st_call(0, SYS_YIELD, 0, 0);
        st_call(1, SYS_IPC_CALL, IT_ARGS(0), 0);
        st_call(2, SYS_HANDLE_CLOSE, (uint32_t)hv[0], 0);
        ht_go(tida, 0, 2);
        if (!it_ends(ha, E_OK)) goto done;
        htab_close(tab, ha); ha = -1;
        if (ht_handles(o1) != 1 || ht_handles(o2) != 1 || kobj_live(KOBJ_REPLY) != calls0) goto done;
    }

    /* 10: a request moving hv[0] down to HRIGHT_WAIT and copying hv[1] with
       HRIGHT_ARM: hv[0] leaves A's table as the call queues, hv[1] stays,
       and each is in transit with a handle count of its own */
    {
        struct kobj *objs[2] = { o1, o2 };
        const uint32_t rights[2] = { ALL, ALL };
        ha = ht_caller(ep, 0, objs, rights, 2, hv, &tida);
        step = 10; if (ha <= 0 || !ht_names(thread_htab(tida), hv[0], o1, ALL, &g0, 0)
                       || !ht_names(thread_htab(tida), hv[1], o2, ALL, &g1, 0)) goto done;
    }
    ht_desc(HT_CDESC(0), 0, hv[0], IPC_HANDLE_MOVE, HRIGHT_WAIT);
    ht_desc(HT_CDESC(0), 1, hv[1], IPC_HANDLE_COPY, HRIGHT_ARM);
    rh[0] = -1; rh[1] = -1; rh[2] = -1;
    /* A calls, then sleeps while this thread looks at its table, and exits 0 */
    st_call(0, SYS_IPC_CALL, IT_ARGS(0), 0);
    st_call(1, SYS_SLEEP, 300, 0);
    st_call(2, SYS_YIELD, 0, 0);
    ht_go(tida, 0, 2);
    if (!it_queued(ep, 1) || ht_names(thread_htab(tida), hv[0], o1, ALL, 0, 0)
        || !ht_names(thread_htab(tida), hv[1], o2, ALL, 0, 0)
        || ht_handles(o1) != 2 || ht_handles(o2) != 3) goto done;

    /* 11: a receive with room for one handle: too small, told how many,
       given no token; both stay in transit */
    step = 11; if (ht_recv(eh, 1) != E_BUFFER_TOO_SMALL || ti->handles != 2 || ti->reply != 0
                   || ti->reply_handles != HT_RHCAP || ep_queued(ep) != 1
                   || ht_handles(o1) != 2 || ht_handles(o2) != 3) goto done;

    /* 12: received: the moved handle keeps its grant, the copy is derived
       from A's */
    step = 12; if (ht_recv(eh, IPC_HANDLES_MAX) != 0 || ti->handles != 2 || ti->reply <= 0) goto done;
    tok = ti->reply; th0 = th[0]; th1 = th[1];
    if (!ht_names(tab, th0, o1, HRIGHT_WAIT, &g, 0) || g != g0
        || !ht_names(tab, th1, o2, HRIGHT_ARM, &g, &p) || p != g1 || g == g1
        || ht_handles(o1) != 2 || ht_handles(o2) != 3) goto done;

    /* 13: reply refusals, each keeping the token and every handle: more
       handles than A takes, a dead handle beside a good one, the token
       itself */
    ht_desc(HT_DESC, 0, th0, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
    ht_desc(HT_DESC, 1, 9999, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
    ht_desc(HT_DESC, 2, t1, IPC_HANDLE_COPY, IPC_RIGHTS_SAME);
    ht_desc(HT_DESC, 3, t2, IPC_HANDLE_COPY, IPC_RIGHTS_SAME);
    step = 13; if (ht_reply(tok, 0, HT_RHCAP + 1) != E_BUFFER_TOO_SMALL
                   || ht_reply(tok, 0, 2) != E_BAD_HANDLE) goto done;
    ht_desc(HT_DESC, 0, tok, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
    if (ht_reply(tok, 0, 1) != E_NOT_SUPPORTED || !ht_names(tab, th0, o1, HRIGHT_WAIT, 0, 0)
        || !thread_is_blocked(tida) || ht_handles(o1) != 2) goto done;
    {
        struct kobj *r = htab_ref(tab, tok, KOBJ_REPLY, HRIGHT_REPLY, 0);
        if (!r) goto done;
        kobj_unref(r);
    }

    /* 14: the reply moves th0 back (its grant still A's first) and copies t2
       with HRIGHT_WAIT; A finds both, then a 0 */
    ht_desc(HT_DESC, 0, th0, IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
    ht_desc(HT_DESC, 1, t2, IPC_HANDLE_COPY, HRIGHT_WAIT);
    data[0] = 'h';
    step = 14; if (ht_reply(tok, 1, 2) != E_OK) goto done;
    tok = -1;
    for (int k = 0; k < 100 && rh[2] == -1; k++) (void)thread_sleep_ms(10);
    if (rh[0] <= 0 || rh[1] <= 0 || rh[2] != 0 || ht_names(tab, th0, o1, HRIGHT_WAIT, 0, 0)
        || ((const uint8_t *)user_ptr(IT_REPLY(0)))[0] != 'h'
        || !ht_names(thread_htab(tida), rh[0], o1, HRIGHT_WAIT, &g, 0) || g != g0
        || !ht_names(thread_htab(tida), rh[1], o2, HRIGHT_WAIT, 0, &p) || p != gt2) goto done;
    th0 = -1;
    if (!it_ends(ha, E_OK) || ht_handles(o1) != 1 || ht_handles(o2) != 2) goto done;
    htab_close(tab, ha); ha = -1;

    /* 15: a queued call whose server goes closes what it carried */
    e2 = ep_create();
    ep2 = e2 > 0 ? ep_from_handle(e2, HRIGHT_RECV, 0) : 0;
    {
        struct kobj *objs[1] = { o1 };
        const uint32_t rights[1] = { ALL };
        hb = ep2 ? ht_caller(ep2, 1, objs, rights, 1, hv, &tidb) : -1;
    }
    step = 15; if (hb <= 0) goto done;
    ht_desc(HT_CDESC(1), 0, hv[0], IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
    ht_go(tidb, 1, 1);
    if (!it_queued(ep2, 1) || ht_handles(o1) != 2) goto done;
    htab_close(tab, e2); e2 = -1;
    if (!it_ends(hb, E_PEER_CLOSED) || ht_handles(o1) != 1) goto done;
    htab_close(tab, hb); hb = -1;
    ep_put(ep2); ep2 = 0;

    /* 16: so does a queued call whose caller is killed */
    {
        struct kobj *objs[1] = { o1 };
        const uint32_t rights[1] = { ALL };
        hb = ht_caller(ep, 1, objs, rights, 1, hv, &tidb);
    }
    step = 16; if (hb <= 0) goto done;
    ht_desc(HT_CDESC(1), 0, hv[0], IPC_HANDLE_MOVE, IPC_RIGHTS_SAME);
    ht_go(tidb, 1, 1);
    if (!it_queued(ep, 1) || ht_handles(o1) != 2 || !kt_kill(hb, tidb, 1)
        || ep_queued(ep) != 0 || ht_handles(o1) != 1) goto done;
    htab_close(tab, hb); hb = -1;

    /* 77-78: a memory object travels (1.3e), with fewer rights, and its
       receiver reads the seals that let it trust the bytes */
    {
        int mrc;
        mb = kmem_create(1, thread_proc(thread_current_id())->job, &mrc);
        step = 77; if (!mb || kmem_seal(mb, MEM_SEAL_WRITE | MEM_SEAL_SEAL) != E_OK) goto done;
        struct kobj *objs[1] = { &mb->hdr };
        const uint32_t rights[1] = { HRIGHT_MAP | HRIGHT_MAP_EXEC | HRIGHT_INSPECT };
        ha = ht_caller(ep, 0, objs, rights, 1, hv, &tida);
        if (ha <= 0) goto done;
        ht_desc(HT_CDESC(0), 0, hv[0], IPC_HANDLE_MOVE, HRIGHT_MAP | HRIGHT_INSPECT);
        ht_go(tida, 0, 1);
        const struct mem_info *mi = (const struct mem_info *)user_ptr(JT_INFO);
        step = 78; if (!it_queued(ep, 1) || ht_recv(eh, 1) != 0 || ti->handles != 1 || ti->reply <= 0) goto done;
        tok = ti->reply; th0 = th[0];
        if (!ht_names(tab, th0, &mb->hdr, HRIGHT_MAP | HRIGHT_INSPECT, 0, 0)
            || jt_info_as(th0, OBJINFO_MEM, sizeof *mi) != (int)sizeof *mi
            || mi->seals != (MEM_SEAL_WRITE | MEM_SEAL_SEAL) || mi->size != PAGE_SIZE
            || ht_reply(tok, 0, 0) != E_OK) goto done;
        tok = -1;
        if (!it_ends(ha, E_OK) || ht_handles(&mb->hdr) != 1) goto done;
        htab_close(tab, ha); ha = -1;
        htab_close(tab, th0); th0 = -1;
        kobj_unref(&mb->hdr); mb = 0;
    }

    /* 17: nothing left behind */
    htab_close(tab, th1); th1 = -1;
    kobj_unref(o1); o1 = 0;
    kobj_unref(o2); o2 = 0;
    htab_close(tab, t1); t1 = -1;
    htab_close(tab, t2); t2 = -1;
    step = 17; if (proc_live() != live0 || usermode_faults() != faults0
                   || syscall_refused_count() != refused0 || kobj_live(KOBJ_REPLY) != calls0
                   || kobj_live(KOBJ_TIMER) != timers0 || kobj_live(KOBJ_MEM) != mems0
                   || ep_in_use_count() != eps0 + 1
                   || sysfile_open_count() != files0) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "ipc handles test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    hf_go = 1;                         /* a helper left behind answers and ends */
    for (int i = 0; hf_tid >= 0 && i < 400 && thread_is_alive(hf_tid); i++) yield();
    {
        int32_t hs[2] = { ha, hb };
        for (int k = 0; k < 2; k++) {
            if (hs[k] <= 0) continue;
            kt_kill_call(hs[k]);
            pt_wait(hs[k], SIGNAL_PROCESS_ENDED, 3000, 0);
            htab_close(tab, hs[k]);
        }
    }
    if (tok > 0) htab_close(tab, tok);
    if (th0 > 0) htab_close(tab, th0);
    if (th1 > 0) htab_close(tab, th1);
    if (o1) kobj_unref(o1);
    if (o2) kobj_unref(o2);
    if (fo) kobj_unref(fo);
    if (mb) kobj_unref(&mb->hdr);
    if (fh >= 0) htab_close(tab, fh);
    if (fb >= 0) htab_close(tab, fb);
    if (t1 > 0) htab_close(tab, t1);
    if (t2 > 0) htab_close(tab, t2);
    if (ep2) ep_put(ep2);
    if (e2 > 0) htab_close(tab, e2);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}

/* ---- Priority through calls (Phase 1.3f) ----
 * The test thread is the server again, at PRIO_NORMAL, and each caller is
 * given its level while it is held. The syscall's refusals and a job's
 * ceiling; a caller above the test thread runs as soon as it is started or
 * answered, and one below waits; a call queued on an endpoint lifts the
 * threads answering its other calls, and a taken one the thread that took
 * it; a reply, a kill and the endpoint closing take the lift off; along a
 * chain, a middle server passes its caller's level on, and a change to it
 * follows. 1 = pass. */
#define PR_MRECV (TEST_CODE_VIRT + 0x800)   /* the middle server's ipc_recv_args */
#define PR_MINFO (TEST_CODE_VIRT + 0x840)   /* its ipc_msg_info */
#define PR_MBUF  (TEST_CODE_VIRT + 0x880)   /* its receive buffer, 64 bytes */
#define PR_MCALL (TEST_CODE_VIRT + 0x900)   /* its ipc_call_args */

/* The test thread's own level and the one it runs at, as the syscall says. */
static int pr_is(uint32_t base, uint32_t prio) {
    return syscall_dispatch(SYS_THREAD_PRIORITY, 0, 0, 0, 0, 0) == (int)(base | prio << 8);
}

static int pr_set(uint32_t level) {
    return syscall_dispatch(SYS_THREAD_SET_PRIORITY, level, 0, 0, 0, 0);
}

/* A caller as it_caller makes one, in slot `i` with a byte of IT_REQ(i),
   given `level` while held and then started. The process handle, or <= 0.
   The blob is copied when a process first runs: one below the test thread
   must have run before the next is made. */
static int32_t pr_caller(struct endpoint *ep, uint32_t i, uint32_t level, int *tid) {
    int32_t h = it_caller(ep, 0, 0, i, 1, IT_REPLY(i), IT_RCAP, 0, 0, tid);
    if (h <= 0) return h;
    ipc_set_base(*tid, level);
    thread_start(*tid);
    return h;
}

/* A held process in `j` whose third call is SYS_THREAD_SET_PRIORITY(level),
   exiting with what that returned. The process handle, or <= 0. */
static int32_t pr_setter(struct job *j, uint32_t level, int *tid) {
    st_len = (uint32_t)(user_blob_calls_end - user_blob_calls_start);
    if (st_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t k = 0; k < st_len; k++) st_image[k] = user_blob_calls_start[k];
    int32_t h = pt_make_in(st_image, st_image + st_len + 3 * 6 * 8, tid, j);
    if (h <= 0) return h;
    st_call(0, SYS_YIELD, 0, 0);
    st_call(1, SYS_YIELD, 0, 0);
    st_call(2, SYS_THREAD_SET_PRIORITY, level, 0);
    return h;
}

/* The middle of a chain, at `level`: it receives one call on `ep1`, calls
   `ep2` with that call's first byte, and exits 0 once it is answered; the
   call it took is left unanswered. Started. The process handle, or <= 0. */
static int32_t pr_middle(struct endpoint *ep1, struct endpoint *ep2, uint32_t level, int *tid) {
    st_len = (uint32_t)(user_blob_calls_end - user_blob_calls_start);
    if (st_len > KT_BLOB_MAX) return E_INTERNAL;
    for (uint32_t k = 0; k < st_len; k++) st_image[k] = user_blob_calls_start[k];
    int32_t h = pt_make_in(st_image, st_image + st_len + 3 * 6 * 8, tid, 0);
    if (h <= 0) return h;
    int32_t r = ep_install_handle(*tid, ep1, HRIGHT_RECV, 0);
    int32_t s = ep_install_send(*tid, ep2, 0, HRIGHT_SEND, 0);
    if (r <= 0 || s <= 0) {
        thread_discard(*tid);
        htab_close(thread_htab(thread_current_id()), h);
        return E_NO_RESOURCES;
    }
    struct ipc_recv_args *ra = (struct ipc_recv_args *)user_ptr(PR_MRECV);
    ra->cap = 64; ra->buf = PR_MBUF; ra->info = PR_MINFO; ra->ep_handle = r;
    ra->handles = 0; ra->handle_cap = 0;
    struct ipc_call_args *ca = (struct ipc_call_args *)user_ptr(PR_MCALL);
    ca->req_len = 1; ca->reply_cap = 0; ca->req = PR_MBUF; ca->reply = 0;
    ca->ep_handle = s; ca->reserved = 0;
    ca->handles = 0; ca->handle_count = 0; ca->reply_handles = 0; ca->reply_handle_cap = 0;
    st_call(0, SYS_IPC_RECV, PR_MRECV, 0);
    st_call(1, SYS_IPC_CALL, PR_MCALL, 0);
    st_call(2, SYS_YIELD, 0, 0);
    ipc_set_base(*tid, level);
    thread_start(*tid);
    return h;
}

/* It raises itself: raising a READY thread asks for a switch (outrank_check),
   and that request, left standing, would hide whether the wake asks too. */
static volatile uint64_t pr_woke_ns;
static volatile int pr_asleep;
static void pr_sleeper(void) {
    ipc_set_base(thread_current_id(), PRIO_NORMAL + 2);
    pr_asleep = 1;
    thread_sleep_ms(20);
    pr_woke_ns = timer_now_ns();
}

int usermode_prio_test(void) {
    paddr_t phys = map_user_page(TEST_CODE_VIRT);
    if (!phys) return 0;
    int me = thread_current_id();
    struct htab *tab = thread_htab(me);
    const struct ipc_msg_info *ti = (const struct ipc_msg_info *)user_ptr(IT_TINFO);
    uint32_t caps0 = thread_current_caps(), base0 = thread_base(me);
    uint32_t live0 = proc_live(), faults0 = usermode_faults(), refused0 = syscall_refused_count();
    uint32_t calls0 = kobj_live(KOBJ_REPLY), jobs0 = job_live();
    int eps0 = ep_in_use_count();
    struct endpoint *ep = 0, *ep2 = 0;
    struct job *jb = 0;
    int32_t eh = -1, e2 = -1, ha = -1, hb = -1, tok = -1, tokb = -1;
    int tida = -1, tidb = -1, step = 0, ok = 0, rc = 0;

    /* 1: the syscall: out of range either side; real-time only with
       GRANT_REALTIME, the level below it without; lowering always */
    step = 1; if (base0 != PRIO_NORMAL || !pr_is(PRIO_NORMAL, PRIO_NORMAL)
                  || pr_set(0) != E_OUT_OF_RANGE || pr_set(PRIO_MAX + 1) != E_OUT_OF_RANGE) goto done;
    thread_set_caps(me, caps0 & ~GRANT_REALTIME);
    if (pr_set(PRIO_REALTIME) != E_ACCESS_DENIED || !pr_is(PRIO_NORMAL, PRIO_NORMAL)
        || pr_set(PRIO_REALTIME - 1) != E_OK || !pr_is(PRIO_REALTIME - 1, PRIO_REALTIME - 1)
        || pr_set(PRIO_MIN) != E_OK || !pr_is(PRIO_MIN, PRIO_MIN)) goto done;
    thread_set_caps(me, caps0 | GRANT_REALTIME);
    if (pr_set(PRIO_MAX) != E_OK || !pr_is(PRIO_MAX, PRIO_MAX)) goto done;
    thread_set_caps(me, caps0 & ~GRANT_REALTIME);
    if (pr_set(PRIO_NORMAL) != E_OK || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;

    /* 2: a job's ceiling: a thread made in it starts there, may set itself
       to it, and is refused the level above */
    jb = job_create(job_root(), 0, 0, PRIO_NORMAL - 3, &rc);
    step = 2; if (!jb) goto done;
    ha = pr_setter(jb, PRIO_NORMAL - 2, &tida);
    if (ha <= 0 || thread_base(tida) != PRIO_NORMAL - 3 || thread_prio(tida) != PRIO_NORMAL - 3) goto done;
    thread_start(tida);
    if (!it_ends(ha, E_ACCESS_DENIED)) goto done;
    htab_close(tab, ha); ha = -1;
    hb = pr_setter(jb, PRIO_NORMAL - 3, &tidb);
    if (hb <= 0) goto done;
    thread_start(tidb);
    if (!it_ends(hb, E_OK)) goto done;
    htab_close(tab, hb); hb = -1;

    /* 3: a caller below the test thread waits while it runs; queued, it
       lifts nobody, since nobody answers the endpoint yet */
    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    step = 3; if (!ep) goto done;
    *(volatile uint8_t *)user_ptr(IT_REQ(0)) = 'L';
    ha = pr_caller(ep, 0, PRIO_NORMAL - 4, &tida);
    if (ha <= 0) goto done;
    yield(); yield();
    if (ep_queued(ep) != 0 || !it_queued(ep, 1) || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;

    /* 4: one above runs at once: queued by the time a syscall returns */
    *(volatile uint8_t *)user_ptr(IT_REQ(1)) = 'H';
    hb = pr_caller(ep, 1, PRIO_REALTIME + 1, &tidb);
    step = 4; if (hb <= 0) goto done;
    (void)syscall_dispatch(SYS_GETPID, 0, 0, 0, 0, 0);
    if (ep_queued(ep) != 2 || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;

    /* 5: answering L's call, the test thread is lifted by H's queued one */
    step = 5; if (it_recv(eh, 64) != 1 || ti->sender != thread_pid(tida) || (tok = ti->reply) <= 0
                  || !pr_is(PRIO_NORMAL, PRIO_REALTIME + 1)) goto done;

    /* 6: and by H's taken one; the reply takes it off, and H, above, has
       ended by the time the reply returns */
    step = 6; if (it_recv(eh, 64) != 1 || ti->sender != thread_pid(tidb) || (tokb = ti->reply) <= 0
                  || !pr_is(PRIO_NORMAL, PRIO_REALTIME + 1)) goto done;
    if (it_reply(tokb, IT_DATA, 0) != E_OK) goto done;
    tokb = -1;
    if (!pr_is(PRIO_NORMAL, PRIO_NORMAL) || pt_wait(hb, SIGNAL_PROCESS_ENDED, 0, 0) != E_OK
        || !it_ends(hb, 0)) goto done;
    htab_close(tab, hb); hb = -1;

    /* 7: a queued caller killed lifts nobody */
    hb = pr_caller(ep, 1, PRIO_MAX, &tidb);
    step = 7; if (hb <= 0 || !it_queued(ep, 1) || !pr_is(PRIO_NORMAL, PRIO_MAX)) goto done;
    if (kt_kill_call(hb) != E_OK || pt_wait(hb, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK
        || ep_queued(ep) != 0 || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;
    htab_close(tab, hb); hb = -1;

    /* 8: nor does a caller killed whose call is taken, its token still held */
    hb = pr_caller(ep, 1, PRIO_MAX, &tidb);
    step = 8; if (hb <= 0 || !it_queued(ep, 1) || it_recv(eh, 64) != 1 || (tokb = ti->reply) <= 0
                  || !pr_is(PRIO_NORMAL, PRIO_MAX)) goto done;
    if (kt_kill_call(hb) != E_OK || pt_wait(hb, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK
        || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;
    htab_close(tab, tokb); tokb = -1;
    htab_close(tab, hb); hb = -1;

    /* 9: the endpoint closing fails a queued caller, and its lift goes; the
       call already taken is still answered */
    hb = pr_caller(ep, 1, PRIO_MAX, &tidb);
    step = 9; if (hb <= 0 || !it_queued(ep, 1) || !pr_is(PRIO_NORMAL, PRIO_MAX)) goto done;
    htab_close(tab, eh); eh = -1;
    if (!it_ends(hb, E_PEER_CLOSED) || !pr_is(PRIO_NORMAL, PRIO_NORMAL)
        || it_reply(tok, IT_DATA, 0) != E_OK || !it_ends(ha, 0)) goto done;
    tok = -1;
    htab_close(tab, ha); ha = -1;
    htab_close(tab, hb); hb = -1;
    ep_put(ep); ep = 0;

    /* 10: a chain: M (below the test thread) receives on one endpoint and
       calls the test thread's other */
    eh = ep_create();
    ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    e2 = ep_create();
    ep2 = e2 > 0 ? ep_from_handle(e2, HRIGHT_RECV, 0) : 0;
    step = 10; if (!ep || !ep2) goto done;
    ha = pr_middle(ep, ep2, PRIO_NORMAL - 4, &tida);
    if (ha <= 0) goto done;
    for (int k = 0; k < 100 && !thread_is_blocked(tida); k++) (void)thread_sleep_ms(10);
    if (!thread_is_blocked(tida)) goto done;

    /* 11: H calls M, which runs at H's level, and calls on */
    *(volatile uint8_t *)user_ptr(IT_REQ(1)) = 'C';
    hb = pr_caller(ep, 1, PRIO_REALTIME, &tidb);
    step = 11; if (hb <= 0 || !it_queued(ep2, 1) || thread_prio(tida) != PRIO_REALTIME
                   || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;

    /* 12: answering M's call, the test thread runs at H's level */
    step = 12; if (it_recv(e2, 64) != 1 || ti->sender != thread_pid(tida) || (tok = ti->reply) <= 0
                   || *(const uint8_t *)user_ptr(IT_TBUF) != 'C'
                   || !pr_is(PRIO_NORMAL, PRIO_REALTIME)) goto done;

    /* 13: H's level, raised and lowered, follows the chain down */
    ipc_set_base(tidb, PRIO_MAX);
    step = 13; if (thread_prio(tida) != PRIO_MAX || !pr_is(PRIO_NORMAL, PRIO_MAX)) goto done;
    ipc_set_base(tidb, PRIO_NORMAL - 3);
    if (thread_prio(tida) != PRIO_NORMAL - 3 || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;

    /* 14: H killed: M is back at its own level, the test thread at its */
    ipc_set_base(tidb, PRIO_REALTIME);
    step = 14; if (!pr_is(PRIO_NORMAL, PRIO_REALTIME) || kt_kill_call(hb) != E_OK
                   || pt_wait(hb, SIGNAL_PROCESS_ENDED, 3000, 0) != E_OK
                   || thread_prio(tida) != PRIO_NORMAL - 4 || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;

    /* 15: M answered ends */
    step = 15; if (it_reply(tok, IT_DATA, 0) != E_OK || !it_ends(ha, 0)) goto done;
    tok = -1;

    /* 16: nothing left behind, nothing lifted */
    htab_close(tab, ha); ha = -1;
    htab_close(tab, hb); hb = -1;
    htab_close(tab, eh); eh = -1;
    htab_close(tab, e2); e2 = -1;
    ep_put(ep); ep = 0;
    ep_put(ep2); ep2 = 0;
    job_put(jb); jb = 0;
    /* 16: a higher thread woken by the timer runs at once, not at the slice's
       end: a kernel thread two above sleeps, and this one spins with
       preemption on and a 500 ms slice, making no call that could yield */
    pr_woke_ns = 0;
    pr_asleep = 0;
    int ts = thread_create("pr_sleep", pr_sleeper);
    for (int i = 0; i < 20 && ts > 0 && !pr_asleep; i++) yield();
    uint64_t t0 = timer_now_ns();
    sched_preempt_enable(500);
    while (!pr_woke_ns && timer_now_ns() - t0 < 400ull * TIMER_NS_PER_TICK) arch_cpu_relax();
    sched_preempt_disable();
    uint64_t woke = pr_woke_ns;
    for (int i = 0; i < 50 && !pr_woke_ns; i++) yield();
    step = 16; if (ts <= 0 || !woke || woke - t0 > 150ull * TIMER_NS_PER_TICK) goto done;

    step = 17; if (ipc_serving_count() != 0 || proc_live() != live0 || usermode_faults() != faults0
                   || syscall_refused_count() != refused0 || kobj_live(KOBJ_REPLY) != calls0
                   || job_live() != jobs0 || ep_in_use_count() != eps0
                   || !pr_is(PRIO_NORMAL, PRIO_NORMAL)) goto done;
    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "priority test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    if (tok > 0) htab_close(tab, tok);
    if (tokb > 0) htab_close(tab, tokb);
    {
        int32_t hs[2] = { ha, hb };
        for (int k = 0; k < 2; k++) {
            if (hs[k] <= 0) continue;
            kt_kill_call(hs[k]);
            pt_wait(hs[k], SIGNAL_PROCESS_ENDED, 3000, 0);
            htab_close(tab, hs[k]);
        }
    }
    if (ep2) ep_put(ep2);
    if (e2 > 0) htab_close(tab, e2);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    if (jb) job_put(jb);
    thread_set_caps(me, caps0);
    ipc_set_base(me, base0);
    unmap_user_page(TEST_CODE_VIRT, phys);
    return ok;
}
