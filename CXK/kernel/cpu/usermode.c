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
int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2) {
    (void)a2;
    switch (num) {
        case SYS_WRITE: {
            /* a1 = pointer to a NUL-terminated string in user memory.
               (Stage 1: we trust it. Real validation comes later.) */
            const char *s = (const char *)a1;
            console_print(s);
            return 0;
        }
        case SYS_EXIT:
            /* return to the kernel. does not return from here. */
            return_to_kernel((int)a1);
            return 0;   /* unreachable */
        default:
            return -1;
    }
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