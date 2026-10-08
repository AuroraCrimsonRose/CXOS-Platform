// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/usermode.c */
/* Aurora Tejeda / CATX Systems */
/* Ring 3 entry, syscall dispatch, and the user-mode tests (Checkpoints 3a/3b). */

#include "usermode.h"
#include "idt.h"
#include "gdt.h"
#include "sched.h"
#include "uid.h"
#include "paging.h"
#include "pmm.h"
#include "vmregion.h"
#include "console.h"
#include "logging.h"
#include "vga.h"
#include "color.h"
#include "fb.h"
#include "caps.h"
#include "handle.h"
#include "spawn.h"
#include "sysfile.h"
#include "cxfs.h"
#include "spawn.h"

#define KERNEL_VBASE 0xC0000000u   /* user half is everything below the higher-half kernel */
#include "ipc.h"
#include "keyboard.h"
#include "mouse.h"
#include "power.h"
#include "netif.h"
#include "timer.h"
#include "rtc.h"
#include "datetime.h"

/* asm entry points (usermode.asm) */
extern int  enter_usermode(uint32_t entry_eip, uint32_t user_esp, uint32_t *save_slot);
extern void syscall_stub(void);
extern void return_to_kernel(int retval, uint32_t *save_slot);

/* ---- v5 user memory --------------------------------------------------------
 * v4 mapped user pages identity (virt==phys) at low physical RAM, ASSUMING that
 * RAM was free. In v5 the pmm owns physical memory, so user pages must be backed
 * by frames ALLOCATED from the pmm and mapped at user-VIRTUAL addresses (low,
 * below the kernel's 0xC0000000, so ring 3 can reach them). The kernel copies
 * the blob in via the user-virtual address - legal because ring 0 may touch
 * PAGE_USER pages, and the mapping is live in the current address space.
 *
 * user virtual layout (per process slot):
 *   code_virt  = R3_BASE + pid * R3_SLOT_SIZE
 *   stack_virt = code_virt + 0x1000
 */
#define R3_BASE       0x00800000u      /* 8 MB: base of user virtual region */
#define R3_SLOT_SIZE  0x00010000u      /* 64 KB virtual slot per process */
#define USER_MSG_OFF  0x800            /* message sits 2KB into the code page */

/* single-test (3a) user virtual addresses (pid-independent fixed slot 0) */
#define TEST_CODE_VIRT   R3_BASE
#define TEST_STACK_VIRT  (R3_BASE + 0x1000)

/* map one fresh pmm frame at virtual `virt` (PAGE_USER). returns the physical
   frame (so it can be freed later), or 0 on failure. */
static uint32_t map_user_page(uint32_t virt) {
    uint32_t phys = (uint32_t)pmm_alloc();
    if (!phys) return 0;
    if (paging_map_user(virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0) { pmm_free((void *)phys); return 0; }
    return phys;
}

static void unmap_user_page(uint32_t virt, uint32_t phys) {
    paging_unmap(virt);
    if (phys) pmm_free((void *)phys);
}

/* ---- syscall pointer validation -------------------------------------------
 * With a privilege boundary in place, the kernel must not blindly dereference
 * pointers handed up from ring 3. Validate against the current process's user
 * region. */

/* Shared body. `need_write` decides which question is actually being asked of
   the page tables - see user_ptr_readable / user_ptr_writable below. */
static int user_ptr_span(uint32_t ptr, uint32_t len, int need_write) {
    if (len == 0) len = 1;
    if (ptr + len < ptr)            return 0;   /* wrap/overflow */
    if (ptr + len > KERNEL_VBASE)   return 0;   /* must lie entirely in the user half */
    /* every page the buffer spans must be present + ring-3 accessible in the
       caller's active address space (its CR3 is live during this syscall). */
    for (uint32_t p = ptr & ~0xFFFu; p < ptr + len; p += 0x1000) {
        if (need_write ? !paging_is_user_writable(p) : !paging_is_user(p)) return 0;
    }
    return 1;
}

/* A buffer the kernel will only READ from. */
int user_ptr_readable(uint32_t ptr, uint32_t len) { return user_ptr_span(ptr, len, 0); }

/* A buffer the kernel will WRITE to.
 *
 * There was one check for both, and it tested presence and the user bit only
 * (security review §4). A read-only user page passed it, and the kernel's write
 * then went through regardless - ring 0 ignores the read-only bit unless CR0.WP
 * is set. So a process could hand a syscall a pointer into its own text and have
 * the kernel scribble on it, which is both a way to defeat W^X from the other
 * side and a way to corrupt a page the process had every reason to think was
 * immutable.
 *
 * The single function is deliberately gone rather than kept as an alias: every
 * call site has to say which access it means, and a new one cannot default to
 * the weaker check by forgetting.
 */
int user_ptr_writable(uint32_t ptr, uint32_t len) { return user_ptr_span(ptr, len, 1); }

/* ---- syscall dispatch (called from syscall_stub) ---- */
int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2) {
    switch (num) {
        case SYS_CONSOLE_WRITE: {
            if (!(thread_current_caps() & GRANT_CONSOLE)) {
                klog_u32("GRANT", SEV_WARN, "console_write DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no GRANT_CONSOLE)");
                return E_PERM;
            }
            uint32_t len = a2;
            if (len == 0) {
                const char *p = (const char *)a1;
                while (len < 0x1000 && user_ptr_readable(a1 + len, 1) && p[len]) len++;
            }
            if (!user_ptr_readable(a1, len ? len : 1)) return -1;
            const char *s = (const char *)a1;
            for (uint32_t i = 0; i < len; i++) console_putc(s[i]);
            return (int)len;
        }
        case SYS_GETPID:
            return thread_current_id();
        case SYS_GETUID:
            return (int)thread_current_uid();
        case SYS_EXIT:
            return_to_kernel((int)a1, thread_current_usave());
            return 0;   /* unreachable */
        case SYS_YIELD:
            yield();
            return 0;

        case SYS_CLOCK: {
            /* Unprivileged: the time of day is not authority. A process that
               could not read it would simply count its own loop iterations. */
            if (!user_ptr_writable(a1, sizeof(struct clock_info))) return E_FAULT;
            struct clock_info *ci = (struct clock_info *)a1;
            ci->ticks = timer_ticks();

            /* epoch 0 means "no usable RTC", which the ABI documents, so a
               caller that needs a real date can tell. Deriving it here rather
               than handing over the raw RTC fields keeps the BCD/century mess
               on this side of the boundary. */
            struct rtc_time t;
            rtc_read(&t);
            struct datetime dt = { t.year, t.month, t.day, t.hour, t.minute, t.second };
            ci->epoch = (uint32_t)datetime_to_epoch(&dt);
            return E_OK;
        }

        case SYS_ARGS: {
            /* Unprivileged: a program asking where its own arguments are.
               This exists so USER_ARGS_BASE does not have to be compiled into
               every program - the address is a 32-bit x86 fact, and a userland
               that knew it would have to be ported alongside the kernel. */
            if (!user_ptr_writable(a1, sizeof(struct args_info))) return E_FAULT;
            struct args_info *ai = (struct args_info *)a1;
            ai->base  = USER_ARGS_BASE;
            ai->count = *(const uint32_t *)USER_ARGS_BASE;
            return E_OK;
        }

        case SYS_SLEEP:
            /* Also unprivileged: waiting is the opposite of a privilege. Note
               this is NOT POWER_SLEEP, which puts the machine into an ACPI
               sleep state and is rightly gated - this blocks one thread and
               leaves everything else running.

               Capped rather than unbounded: a sleep is a promise to come back,
               and one that never does is indistinguishable from a hang. */
            if (a1 > SLEEP_MAX_MS) return E_RANGE;
            thread_sleep_ms(a1);
            return E_OK;

        case SYS_SPAWN:
            if (!(thread_current_caps() & GRANT_SPAWN)) return E_PERM;
            return sys_spawn((const struct spawn_args *)a1);

        case SYS_EXEC_PATH:
            if (!(thread_current_caps() & GRANT_SPAWN)) return E_PERM;
            return sys_exec_path((const char *)a1, (const struct spawn_args *)a2);

        case SYS_FILE_OP:
            if (!(thread_current_caps() & GRANT_DISK)) {
                klog_u32("GRANT", SEV_WARN, "file_op DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no GRANT_DISK)");
                return E_PERM;
            }
            return sys_file_op((const struct file_op_args *)a1);

        case SYS_FB_OP:
            if (!(thread_current_caps() & GRANT_FRAMEBUFFER)) {
                klog_u32("GRANT", SEV_WARN, "fb_op DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no GRANT_FRAMEBUFFER)");
                return E_PERM;
            }
            return sys_fb_op((const struct fb_op_args *)a1);

        case SYS_INPUT_READ:
            /* unprivileged: reading your own keyboard input.
               a1==0 -> block until a key; a1==1 -> return now (0 if none). */
            if (a1 == 1) return (int)(unsigned char)keyboard_getchar();
            return (int)(unsigned char)keyboard_getchar_blocking();

        case SYS_NET_OP:
            if (!(thread_current_caps() & GRANT_NET)) {
                klog_u32("GRANT", SEV_WARN, "net DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no GRANT_NET)");
                return E_PERM;
            }
            return sys_net_op((const struct net_op_args *)a1);

        case SYS_MOUSE_READ: {
            /* unprivileged, same reasoning as keyboard input: a process reading
               the pointer it is already being shown */
            struct mouse_state *ms = (struct mouse_state *)a1;
            if (!user_ptr_writable((uint32_t)ms, sizeof *ms)) return E_FAULT;
            ms->x       = mouse_x();
            ms->y       = mouse_y();
            ms->buttons = mouse_buttons();
            ms->seq     = mouse_seq();
            return mouse_present() ? 1 : 0;
        }

        case SYS_POWER:
            if (!(thread_current_caps() & GRANT_POWER)) {
                klog_u32("GRANT", SEV_WARN, "power DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no GRANT_POWER)");
                return E_PERM;
            }
            if (a1 == POWER_REBOOT)   power_reboot();     /* does not return */
            if (a1 == POWER_SHUTDOWN) power_shutdown();   /* does not return if ACPI S5 works */
            if (a1 == POWER_SLEEP)  { power_sleep(a2); return 0; }
            return E_INVAL;

        case SYS_MEM_OP: {
            /* Unprivileged, like the clock and the sleep: a program that could
               not obtain memory would not be contained, it would be unable to
               run. What bounds it is the per-process quota inside vm_map, not
               a grant bit every program would have to hold. */
            if (!user_ptr_writable(a1, sizeof(struct mem_op_args))) return E_FAULT;
            struct mem_op_args *m = (struct mem_op_args *)a1;
            int pid = thread_current_id();
            switch (m->op) {
                case MEM_OP_MAP:   return vm_map(pid, m);
                case MEM_OP_UNMAP: return vm_unmap(pid, m->addr, m->length);
                case MEM_OP_INFO:  return vm_info(pid, m);
                default:           return E_INVAL;
            }
        }

        case SYS_IPC_CALL:
            return ipc_call((const struct ipc_call_args *)a1);

        case SYS_IPC_RECV:
            return ipc_recv((const struct ipc_recv_args *)a1);

        case SYS_IPC_REPLY:
            return ipc_reply((const struct ipc_reply_args *)a1);

        case SYS_EP_CREATE:
            if (!(thread_current_caps() & GRANT_ENDPOINT)) return E_PERM;
            return ep_create();

        case SYS_HANDLE_CLOSE:
            return thread_handle_close(thread_current_id(), (int)a1);

        default:
            return E_NOSYS;
    }
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
    /* install the syscall gate: int 0x80, DPL=3 so ring 3 can invoke it */
    idt_set_user_gate(0x80, (uint32_t)syscall_stub);
    sysfile_init();   /* registers the HANDLE_FILE releaser */
    ipc_init();       /* registers the HANDLE_ENDPOINT releaser */
}

/* ---- Checkpoint 3a: the minimal single-process ring-3 round-trip ---- */
int usermode_test(void) {
    /* back the code + stack pages with real pmm frames, mapped user-accessible */
    uint32_t code_phys  = map_user_page(TEST_CODE_VIRT);
    uint32_t stack_phys = map_user_page(TEST_STACK_VIRT);
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
    char *umsg = (char *)(TEST_CODE_VIRT + USER_MSG_OFF);
    uint32_t i = 0;
    for (; user_msg[i] && i < 0x100; i++) umsg[i] = user_msg[i];
    umsg[i] = '\0';

    /* seed the message pointer at the top of the user stack ([esp] on entry) */
    uint32_t ustack_top = TEST_STACK_VIRT + 0x1000 - 16;
    *(uint32_t *)ustack_top = (uint32_t)umsg;

    /* dev test runs as a SYSTEM ring-3 helper: grant it console so the gated
       SYS_WRITE works (restored after). */
    int      tme  = thread_current_id();
    uint32_t tsav = thread_current_caps();
    thread_set_caps(tme, GRANT_CONSOLE);
    /* enter ring 3; returns when the routine SYS_EXITs */
    int rc = enter_usermode(TEST_CODE_VIRT, ustack_top, thread_current_usave());
    thread_set_caps(tme, tsav);

    /* reclaim the user pages */
    unmap_user_page(TEST_CODE_VIRT, code_phys);
    unmap_user_page(TEST_STACK_VIRT, stack_phys);
    return rc;
}


/* ---- file syscall test -----------------------------------------------------
 * Drives SYS_FILE_OP the way ring 3 does, short of the int 0x80 gate itself
 * (which the other syscalls already prove). The point of doing it from here
 * rather than from ktest.c is map_user_page: the args and buffers have to live
 * in a PAGE_USER mapping or user_ptr_readable rejects them, which is exactly the
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

static void ft_str(uint32_t at, const char *s) {
    char *d = (char *)at;
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
    char *d = (char *)at;
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
    a->path   = (const char *)path;
    a->path2  = (const char *)FT_PATH2;
    a->data   = (void *)data;
    a->len    = len;
    a->off    = off;
    a->flags  = flags;
    return syscall_dispatch(SYS_FILE_OP, FT_ARGS, 0);
}

int usermode_file_test(void) {
    if (!cxfs_is_mounted()) return 1;         /* nothing mounted - skip */

    uint32_t phys = map_user_page(TEST_CODE_VIRT);
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
    if (ft_call(FILE_OP_READ, h, 0, FT_DATA, 4, 0, 0) != E_BADF) goto done;

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
    if (ft_call(FILE_OP_WRITE, h, 0, FT_DATA, 2, 0, 0) != E_PERM) goto done;
    step++;
    if (ft_call(FILE_OP_CLOSE, h, 0, 0, 0, 0, 0) != E_OK) goto done;

    /* --- a missing file is E_NOENT without FOPEN_CREATE --- */
    ft_strp(FT_PATH, scratch, "/kt_absent.txt");
    step++;
    if (ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ) != E_NOENT) goto done;

    /* --- a bad user pointer is caught, not dereferenced --- */
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (ft_call(FILE_OP_STAT, 0, FT_PATH, 0xC0001000u, 0, 0, 0) != E_FAULT) goto done;
    step++;
    if (ft_call(FILE_OP_OPEN, 0, 0xC0001000u, 0, 0, 0, FOPEN_READ) != E_FAULT) goto done;

    /* --- directories, cwd, and relative paths --- */
    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (ft_call(FILE_OP_MKDIR, 0, FT_PATH, 0, 0, 0, 0) != E_OK) goto done;
    step++;
    if (ft_call(FILE_OP_MKDIR, 0, FT_PATH, 0, 0, 0, 0) != E_EXIST) goto done;
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
    if (ft_call(FILE_OP_UNLINK, 0, FT_PATH, 0, 0, 0, 0) != E_INVAL) goto done;  /* not empty */

    /* --- GRANT_DISK really is the gate --- */
    thread_set_caps(me, 0);
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (ft_call(FILE_OP_STAT, 0, FT_PATH, FT_STAT, 0, 0, 0) != E_PERM) goto done;
    thread_set_caps(me, GRANT_DISK);

    /* --- exec_path refuses everything it should ---
     * The accept path needs a genuinely signed CXEX, which only exists in a
     * SIGN=ON build, so what is checked here is every way it must say no. That
     * is the half that matters: a verifier that never refuses is not one.
     */
    thread_set_caps(me, GRANT_DISK | GRANT_SPAWN);
    struct spawn_args *sa = (struct spawn_args *)FT_STAT;   /* reuse the page */
    sa->image = 0; sa->image_len = 0; sa->name = (const char *)FT_PATH;
    sa->broker_endpoint = -1; sa->caps = 0;
    sa->args = 0; sa->args_len = 0;

    ft_strp(FT_PATH, scratch, "/kt_absent.xuex");
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_NOENT) goto done;

    ft_strp(FT_PATH, scratch, "/kt_dir");
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_ISDIR) goto done;

    /* a real file whose contents are not a CXEX at all: the signature check
       must refuse it rather than the loader trying to run the bytes */
    ft_strp(FT_PATH, scratch, "/kt_sys.txt");
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_PERM) goto done;

    /* a bad path pointer is caught, not dereferenced */
    step++;
    if (sys_exec_path((const char *)0xC0001000u, sa) != E_FAULT) goto done;
    step++;
    if (sys_exec_path((const char *)FT_PATH, (const struct spawn_args *)0xC0001000u) != E_FAULT) goto done;

    /* --- clock and sleep, through the real dispatcher ---
     * Both are unprivileged, so they must work with caps stripped to nothing -
     * that is the assertion, not an afterthought. A future edit that gates
     * either one behind a capability breaks here rather than in whatever
     * service was relying on being able to wait. */
    thread_set_caps(me, 0);
    step++;
    if (syscall_dispatch(SYS_CLOCK, FT_DATA, 0) != E_OK) goto done;
    step++;
    if (((uint32_t *)FT_DATA)[0] == 0) goto done;     /* ticks must have advanced */
    step++;
    if (syscall_dispatch(SYS_SLEEP, 1, 0) != E_OK) goto done;
    thread_set_caps(me, GRANT_DISK | GRANT_SPAWN);

    /* a bad clock pointer is caught, not written through */
    step++;
    if (syscall_dispatch(SYS_CLOCK, 0xC0001000u, 0) != E_FAULT) goto done;
    /* and a sleep longer than the cap is refused rather than silently clamped:
       a caller that asked for a day and got an hour would never know */
    step++;
    if (syscall_dispatch(SYS_SLEEP, SLEEP_MAX_MS + 1, 0) != E_RANGE) goto done;

    /* --- argument blobs the kernel must refuse ---
     * check_args runs before anything touches the disk, so the path here is
     * irrelevant - each of these must fail on the blob alone.
     *
     * The unterminated case is the one worth having: build_args_page counts
     * arguments by counting terminators, so a blob whose last byte is not one
     * would leave the final string running off the end of what was copied.
     * The check that stops it is one line, and nothing else would catch it. */
    sa->args = (const char *)0xC0001000u; sa->args_len = 8;
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_FAULT) goto done;

    ft_str(FT_DATA, "noterm");
    sa->args = (const char *)FT_DATA; sa->args_len = 6;   /* stops before the NUL */
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_INVAL) goto done;

    sa->args_len = USER_ARGS_MAX + 1;                     /* wider than the page */
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_RANGE) goto done;

    sa->args = 0; sa->args_len = 4;                       /* a length with no blob */
    step++;
    if (sys_exec_path((const char *)FT_PATH, sa) != E_INVAL) goto done;

    sa->args = 0; sa->args_len = 0;

    /* and GRANT_SPAWN is the gate: the dispatcher checks it, so go through it */
    thread_set_caps(me, GRANT_DISK);
    step++;
    if (syscall_dispatch(SYS_EXEC_PATH, FT_PATH, (uint32_t)sa) != E_PERM) goto done;
    thread_set_caps(me, GRANT_DISK);

    /* --- the reaper releases handles a process never closed --- */
    h = ft_call(FILE_OP_OPEN, 0, FT_PATH, 0, 0, 0, FOPEN_READ);
    step++;
    if (h < 0) goto done;
    step++;
    if (sysfile_open_count() != base + 1) goto done;
    handle_release_all(thread_handle_table(me), CXK_MAX_HANDLES);
    step++;
    if (sysfile_open_count() != base) goto done;

    ok = 1;

done:
    if (!ok) klog_u32("KTEST", SEV_ERR, "file syscall test failed at step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    /* tidy up whatever got made, so a re-run starts from the same state */
    thread_set_caps(me, GRANT_DISK);
    handle_release_all(thread_handle_table(me), CXK_MAX_HANDLES);
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

/* ---- Checkpoint 3b: scheduler-integrated ring-3 processes ---- */
struct ring3_setup {
    const void *blob;
    uint32_t    blob_len;
    const char *msg;
    uint32_t    code_virt;
    uint32_t    stack_virt;
    uint32_t    code_phys;     /* pmm frame backing the code page (for free) */
    uint32_t    stack_phys;    /* pmm frame backing the stack page (for free) */
};
static struct ring3_setup r3[MAX_THREADS];

static void process_trampoline(void) {
    int pid = thread_current_id();
    struct ring3_setup *s = &r3[pid];

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
    uint8_t *dst = (uint8_t *)s->code_virt;
    const uint8_t *src = (const uint8_t *)s->blob;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    char *umsg = (char *)(s->code_virt + USER_MSG_OFF);
    uint32_t i = 0;
    if (s->msg) { for (; s->msg[i] && i < 0x100; i++) umsg[i] = s->msg[i]; }
    umsg[i] = '\0';

    uint32_t ustack_top = s->stack_virt + 0x1000 - 16;
    *(uint32_t *)ustack_top = (uint32_t)umsg;

    enter_usermode(s->code_virt, ustack_top, thread_current_usave());

    /* user routine SYS_EXITed: reclaim its pages, then leave the scheduler. */
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
    r3[pid].blob       = blob;
    r3[pid].blob_len   = blob_len;
    r3[pid].msg        = msg;
    r3[pid].code_virt  = R3_BASE + (uint32_t)pid * R3_SLOT_SIZE;
    r3[pid].stack_virt = r3[pid].code_virt + 0x1000;
    r3[pid].code_phys  = 0;
    r3[pid].stack_phys = 0;
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
 * panicking the kernel - a buggy user program can't take the system down. */
static void usermode_fault(struct registers *r) {
    int pid = thread_current_id();

    klog_u32("USERMODE", SEV_ERR, "user process ", (uint32_t)pid, LOG_COLOR_VALUE, " faulted");
    klog_child_u32("exception: ", r->int_no, LOG_COLOR_VALUE, "");
    klog_child("process terminated");
    thread_exit();   /* never returns */
}

void usermode_register_fault_handler(void) {
    set_user_fault_hook(usermode_fault);
}