/* /CXK/kernel/cpu/usermode.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Ring 3 entry, syscall dispatch, and the user-mode tests (Checkpoints 3a/3b). */

#include "usermode.h"
#include "idt.h"
#include "gdt.h"
#include "sched.h"
#include "uid.h"
#include "paging.h"
#include "pmm.h"
#include "console.h"
#include "logging.h"
#include "vga.h"
#include "color.h"
#include "fb.h"
#include "caps.h"
#include "handle.h"
#include "spawn.h"

#define KERNEL_VBASE 0xC0000000u   /* user half is everything below the higher-half kernel */
#include "ipc.h"
#include "keyboard.h"
#include "mouse.h"
#include "power.h"
#include "netif.h"

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
    paging_map(virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
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

int user_ptr_ok(uint32_t ptr, uint32_t len) {
    if (len == 0) len = 1;
    if (ptr + len < ptr)            return 0;   /* wrap/overflow */
    if (ptr + len > KERNEL_VBASE)   return 0;   /* must lie entirely in the user half */
    /* every page the buffer spans must be present + ring-3 accessible in the
       caller's active address space (its CR3 is live during this syscall). */
    for (uint32_t p = ptr & ~0xFFFu; p < ptr + len; p += 0x1000) {
        if (!paging_is_user(p)) return 0;
    }
    return 1;
}

/* ---- syscall dispatch (called from syscall_stub) ---- */
int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2) {
    switch (num) {
        case SYS_CONSOLE_WRITE: {
            if (!(thread_current_caps() & CAP_CONSOLE)) {
                klog_u32("CAP", SEV_WARN, "console_write DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no CAP_CONSOLE)");
                return E_PERM;
            }
            uint32_t len = a2;
            if (len == 0) {
                const char *p = (const char *)a1;
                while (len < 0x1000 && user_ptr_ok(a1 + len, 1) && p[len]) len++;
            }
            if (!user_ptr_ok(a1, len ? len : 1)) return -1;
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

        case SYS_SPAWN:
            if (!(thread_current_caps() & CAP_SPAWN)) return E_PERM;
            return sys_spawn((const struct spawn_args *)a1);

        case SYS_FB_OP:
            if (!(thread_current_caps() & CAP_FRAMEBUFFER)) {
                klog_u32("CAP", SEV_WARN, "fb_op DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no CAP_FRAMEBUFFER)");
                return E_PERM;
            }
            return sys_fb_op((const struct fb_op_args *)a1);

        case SYS_INPUT_READ:
            /* unprivileged: reading your own keyboard input.
               a1==0 -> block until a key; a1==1 -> return now (0 if none). */
            if (a1 == 1) return (int)(unsigned char)keyboard_getchar();
            return (int)(unsigned char)keyboard_getchar_blocking();

        case SYS_NET_OP:
            if (!(thread_current_caps() & CAP_NET)) {
                klog_u32("CAP", SEV_WARN, "net DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no CAP_NET)");
                return E_PERM;
            }
            return sys_net_op((const struct net_op_args *)a1);

        case SYS_MOUSE_READ: {
            /* unprivileged, same reasoning as keyboard input: a process reading
               the pointer it is already being shown */
            struct mouse_state *ms = (struct mouse_state *)a1;
            if (!user_ptr_ok((uint32_t)ms, sizeof *ms)) return E_FAULT;
            ms->x       = mouse_x();
            ms->y       = mouse_y();
            ms->buttons = mouse_buttons();
            ms->seq     = mouse_seq();
            return mouse_present() ? 1 : 0;
        }

        case SYS_POWER:
            if (!(thread_current_caps() & CAP_POWER)) {
                klog_u32("CAP", SEV_WARN, "power DENIED for pid ", (uint32_t)thread_current_id(), LOG_COLOR_VALUE, " (no CAP_POWER)");
                return E_PERM;
            }
            if (a1 == POWER_REBOOT)   power_reboot();     /* does not return */
            if (a1 == POWER_SHUTDOWN) power_shutdown();   /* does not return if ACPI S5 works */
            if (a1 == POWER_SLEEP)  { power_sleep(a2); return 0; }
            return E_INVAL;

        case SYS_IPC_CALL:
            return ipc_call((const struct ipc_call_args *)a1);

        case SYS_IPC_RECV:
            return ipc_recv((const struct ipc_recv_args *)a1);

        case SYS_IPC_REPLY:
            return ipc_reply((const struct ipc_reply_args *)a1);

        case SYS_EP_CREATE:
            if (!(thread_current_caps() & CAP_ENDPOINT)) return E_PERM;
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
    thread_set_caps(tme, CAP_CONSOLE);
    /* enter ring 3; returns when the routine SYS_EXITs */
    int rc = enter_usermode(TEST_CODE_VIRT, ustack_top, thread_current_usave());
    thread_set_caps(tme, tsav);

    /* reclaim the user pages */
    unmap_user_page(TEST_CODE_VIRT, code_phys);
    unmap_user_page(TEST_STACK_VIRT, stack_phys);
    return rc;
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

int process_create_ring3(const char *name,
                         const void *blob, uint32_t blob_len,
                         const char *msg) {
    int pid = thread_create(name, process_trampoline);
    if (pid < 0) return -1;
    if (thread_alloc_kstack(pid) < 0) return -1;
    r3[pid].blob       = blob;
    r3[pid].blob_len   = blob_len;
    r3[pid].msg        = msg;
    r3[pid].code_virt  = R3_BASE + (uint32_t)pid * R3_SLOT_SIZE;
    r3[pid].stack_virt = r3[pid].code_virt + 0x1000;
    r3[pid].code_phys  = 0;
    r3[pid].stack_phys = 0;
    thread_mark_user(pid);
    thread_set_caps(pid, CAP_CONSOLE);   /* SYSTEM ring-3 helper: may write console */
    klog_u32("RING3", SEV_INFO, "SYSMODE CALL - pid ", (uint32_t)pid, LOG_COLOR_VALUE, "");
    /* runs as SYSTEM: this is the kernel launching a ring-3 helper as the
       machine identity. To launch on behalf of a human user, use
       process_create_ring3_as_user(). */
    return pid;
}

/* Launch a ring-3 process owned by a specific user. Enforces the core identity
   invariant: a user is ALWAYS UID >= 1 - launching a user process as UID 0
   (SYSTEM) is rejected. Returns the pid, or -1 (incl. if uid == SYSTEM). */
int process_create_ring3_as_user(const char *name, const void *blob, uint32_t blob_len, const char *msg, uint32_t uid) {
    if (!uid_is_user(uid)) return -1;   /* user can never be UID 0 / invalid */
    int pid = process_create_ring3(name, blob, blob_len, msg);
    if (pid < 0) return -1;
    /* pid is already marked is_user by process_create_ring3, so thread_set_uid's
       guard would block UID 0 here too - but uid is validated >= 1 above. */
    thread_set_uid(pid, uid);
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