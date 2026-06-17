/* /CXK/kernel/cpu/usermode.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Ring 3 entry, syscall dispatch, and the Stage-1 user-mode test. */

#include "usermode.h"
#include "idt.h"
#include "gdt.h"
#include "paging.h"
#include "console.h"
#include "vga.h"

/* asm entry points */
extern int  enter_usermode(uint32_t entry_eip, uint32_t user_esp);
extern void syscall_stub(void);
extern void return_to_kernel(int retval);

/* dedicated user pages (separate from kernel memory, marked PAGE_USER).
   chosen in the identity-mapped low region, clear of other DMA users. */
#define USER_CODE_ADDR   0x00800000u    /* 8 MB: user code page */
#define USER_STACK_ADDR  0x00801000u    /* 9th page: user stack page */
#define USER_STACK_TOP   (USER_STACK_ADDR + 0x1000 - 16)

/* The syscall dispatcher (called from syscall_stub).
   num = syscall number, a1/a2 = args. Returns the syscall's result. */
/* validate that a user-supplied buffer [ptr, ptr+len) lies within the mapped
   user region. With a privilege boundary now in place, the kernel must not
   blindly dereference pointers handed up from ring 3. (Stage 1: a single fixed
   user region; per-process address-space checks come with the process model.) */
/* forward decl: per-process user region lookup (defined with the process code) */
static int current_user_region(uint32_t *lo, uint32_t *hi);

static int user_ptr_ok(uint32_t ptr, uint32_t len) {
    uint32_t lo, hi;
    if (!current_user_region(&lo, &hi)) {
        /* legacy single-region path (usermode_test) */
        lo = USER_CODE_ADDR;
        hi = USER_STACK_ADDR + 0x1000;
    }
    if (len > 0x2000) return 0;
    if (ptr < lo || ptr >= hi) return 0;
    if (ptr + len < ptr) return 0;            /* overflow */
    if (ptr + len > hi) return 0;
    return 1;
}

/* The syscall dispatcher (called from syscall_stub).
   num = syscall number, a1/a2 = args. Returns the syscall's result. */
int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2) {
    switch (num) {
        case SYS_WRITE: {
            /* a1 = pointer to a string in user memory, a2 = length (0 = scan to
               NUL, bounded). Validate the pointer is inside the user region
               before touching it. */
            uint32_t len = a2;
            if (len == 0) {
                /* bounded NUL scan within the user region */
                const char *p = (const char *)a1;
                while (len < 0x1000 && user_ptr_ok(a1 + len, 1) && p[len]) len++;
            }
            if (!user_ptr_ok(a1, len ? len : 1)) return -1;
            const char *s = (const char *)a1;
            for (uint32_t i = 0; i < len; i++) console_putc(s[i]);
            return (int)len;
        }
        case SYS_GETPID:
            /* no process model yet; everything is "process 0" for now. */
            return 0;
        case SYS_EXIT:
            /* return to the kernel. does not return from here. */
            return_to_kernel((int)a1);
            return 0;   /* unreachable */
        default:
            return -1;
    }
    (void)a2;
}

/* the position-independent ring-3 routine, defined in usermode.asm */
extern uint8_t user_blob_start[];
extern uint8_t user_blob_end[];

/* the message the user routine prints (placed in the user code page too, so
   it's user-accessible). */
static const char user_msg[] = "  [ring3] hello from user mode via syscall!\n";

void usermode_init(void) {
    /* install the syscall gate: int 0x80, DPL=3 so ring 3 can call it */
    idt_set_user_gate(0x80, (uint32_t)syscall_stub);
}

int usermode_test(void) {
    /* map a user code page and a user stack page (PAGE_USER so ring 3 can touch
       them). identity-mapped: virt == phys here. */
    paging_map(USER_CODE_ADDR,  USER_CODE_ADDR,  PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    paging_map(USER_STACK_ADDR, USER_STACK_ADDR, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    /* copy the position-independent user routine into the user code page */
    uint32_t len = (uint32_t)(user_blob_end - user_blob_start);
    if (len == 0 || len > 0x800) len = 0x800;
    uint8_t *dst = (uint8_t *)USER_CODE_ADDR;
    for (uint32_t i = 0; i < len; i++) dst[i] = user_blob_start[i];

    /* place the message string in the user code page, just after the routine,
       so it's in user-accessible memory. */
    char *umsg = (char *)(USER_CODE_ADDR + 0x800);
    uint32_t i = 0;
    for (; user_msg[i] && i < 0x100; i++) umsg[i] = user_msg[i];
    umsg[i] = '\0';

    /* seed the top of the user stack with the message pointer; the routine
       reads it via `mov ebx, [esp]`. */
    uint32_t *ustack = (uint32_t *)USER_STACK_TOP;
    *ustack = (uint32_t)umsg;

    /* NOTE: the TSS ring-0 stack (esp0) is a DEDICATED stack set up in
       gdt_init() - deliberately NOT the main kernel stack, so a syscall from
       ring 3 doesn't clobber the frame enter_usermode saves below. Do not
       override it here. */

    /* enter ring 3 at the copied routine; returns when the routine SYS_EXITs */
    return enter_usermode(USER_CODE_ADDR, USER_STACK_TOP);
}

/* ---- scheduler-integrated ring-3 processes (Checkpoint 3a.2) ----
 * A ring-3 process is a scheduler thread whose entry is process_trampoline().
 * The trampoline maps this process's user pages, copies in its code+message,
 * and drops to ring 3 via enter_usermode. When the user routine SYS_EXITs,
 * enter_usermode returns and the trampoline calls thread_exit() (scheduler
 * reaps it).
 *
 * 3a.2 scope: cooperative (no preemption while in ring 3), so the dedicated
 * TSS esp0 stack set in gdt_init() suffices - only one process is ever mid-
 * syscall at a time. Per-process esp0 comes in 3a.3 (preemptible processes).
 */
#include "sched.h"

struct ring3_setup {
    const void *blob;
    uint32_t    blob_len;
    const char *msg;
    uint32_t    code_addr;
    uint32_t    stack_addr;
};
static struct ring3_setup r3[MAX_THREADS];

#define R3_BASE      0x00800000u
#define R3_SLOT_SIZE 0x00010000u   /* 64 KB per process slot */

/* report the [lo, hi) user-accessible region of the CURRENT process, if it is a
   ring-3 process with a slot assigned. returns 0 if not (use legacy region). */
static int current_user_region(uint32_t *lo, uint32_t *hi) {
    int pid = thread_current_id();
    if (pid <= 0 || pid >= MAX_THREADS) return 0;
    if (r3[pid].code_addr == 0) return 0;
    *lo = r3[pid].code_addr;
    *hi = r3[pid].stack_addr + 0x1000;
    return 1;
}

static void process_trampoline(void) {
    int pid = thread_current_id();
    struct ring3_setup *s = &r3[pid];

    paging_map(s->code_addr,  s->code_addr,  PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    paging_map(s->stack_addr, s->stack_addr, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    uint32_t len = s->blob_len;
    if (len == 0 || len > 0x800) len = 0x800;
    uint8_t *dst = (uint8_t *)s->code_addr;
    const uint8_t *src = (const uint8_t *)s->blob;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    char *umsg = (char *)(s->code_addr + 0x800);
    uint32_t i = 0;
    if (s->msg) { for (; s->msg[i] && i < 0x100; i++) umsg[i] = s->msg[i]; }
    umsg[i] = '\0';

    uint32_t ustack_top = s->stack_addr + 0x1000 - 16;
    *(uint32_t *)ustack_top = (uint32_t)umsg;

    enter_usermode(s->code_addr, ustack_top);   /* -> ring 3; returns on SYS_EXIT */

    thread_exit();   /* never returns */
}

int process_create_ring3(const char *name,
                         const void *blob, uint32_t blob_len,
                         const char *msg) {
    int pid = thread_create(name, process_trampoline);
    if (pid < 0) return -1;
    r3[pid].blob       = blob;
    r3[pid].blob_len   = blob_len;
    r3[pid].msg        = msg;
    r3[pid].code_addr  = R3_BASE + (uint32_t)pid * R3_SLOT_SIZE;
    r3[pid].stack_addr = R3_BASE + (uint32_t)pid * R3_SLOT_SIZE + 0x1000;
    thread_mark_user(pid);
    return pid;
}

/* ---- user fault handler (registered with the IDT) ----
 * Called when a CPU exception occurs while in ring 3. Instead of panicking the
 * kernel, we kill the offending process and let the scheduler move on. This is
 * what keeps a buggy user program from taking down the whole system.
 */
static void usermode_fault(struct registers *r) {
    int pid = thread_current_id();
    console_set_color(VGA_BROWN, VGA_BLACK);
    console_print("\n[kernel] user process ");
    console_print_dec((uint32_t)pid);
    console_print(" faulted (exc ");
    console_print_dec(r->int_no);
    console_print(") - terminated.\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* terminate the faulting process; thread_exit switches away and never
       returns, so the kernel survives and other processes continue. */
    thread_exit();
    /* not reached */
}

void usermode_register_fault_handler(void) {
    set_user_fault_hook(usermode_fault);
}