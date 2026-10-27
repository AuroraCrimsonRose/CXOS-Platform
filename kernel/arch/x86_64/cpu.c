// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/cpu.c */
/* Aurora Tejeda / CATX Systems */
/* arch/cpu.h on x86-64: the GDT, the TSS and its RSP0, the IDT, the feature
   table, and a new thread's first frame; and arch/usermode.h's gate, which
   is syscall/sysret rather than x86-32's int 0x80 (usermode.asm). */

#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/usermode.h"
#include "arch/x86/msr.h"
#include "arch/x86/cpufeat.h"
#include "gdt.h"
#include "idt.h"
#include "logging.h"

void arch_cpu_init(void) {
    gdt64_init();
    klog("GDT", SEV_OK, "GDT + TSS installed (double fault on IST 1)");
    idt64_init();
    klog("IDT", SEV_OK, "installed (exceptions, IRQs 0-15, MSI pool)");
    /* Before anything that might ask what the CPU has. */
    cpufeat_init();
}

extern void syscall64_entry(void);   /* usermode.asm */

void arch_syscall_gate_init(void) {
    wrmsr(MSR_STAR, STAR_VALUE);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall64_entry);
    wrmsr(MSR_FMASK, FMASK_VALUE);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void arch_syscall_stub(uint8_t *dst, uint32_t nr) {
    dst[0] = 0xB8;                               /* mov eax, nr */
    dst[1] = (uint8_t)nr;         dst[2] = (uint8_t)(nr >> 8);
    dst[3] = (uint8_t)(nr >> 16); dst[4] = (uint8_t)(nr >> 24);
    dst[5] = 0x0F; dst[6] = 0x05;                /* syscall */
    dst[7] = 0xC3;                               /* ret */
}

void arch_kernel_stack_set(uintptr_t top) { gdt64_set_rsp0(top); }

uintptr_t arch_kernel_stack_default(void) { return gdt64_default_rsp0(); }

uintptr_t arch_kernel_stack_current(void) { return gdt64_rsp0(); }

void arch_run_on_stack(uintptr_t top, void (*fn)(void)) {
    /* Fixed registers, as on x86-32: the compiler may hand out rbp, which the
       xor clears before the call reads it. `top` is 16-byte aligned, so fn is
       entered as the ABI expects, 8 below a multiple of 16. */
    __asm__ volatile ("mov %0, %%rsp\n\t"
                      "xor %%ebp, %%ebp\n\t"      /* ends a panic's stack trace here */
                      "call *%1"
                      : : "a"(top), "c"(fn) : "memory");
    __builtin_unreachable();
}

uintptr_t arch_thread_frame(uintptr_t stack_top, void (*launch)(void)) {
    /* context_switch (switch.asm) saves pushfq, rbx, rbp, r12-r15, so from
       the saved RSP upward the frame is
       [r15][r14][r13][r12][rbp][rbx][rflags][return], restored with the
       mirror pops, popfq and ret. Above the return sits a zero where launch's
       own return address would be: launch then starts with RSP 8 below a
       multiple of 16, as after a call, and a stack trace ends there. */
    uint64_t *sp = (uint64_t *)(stack_top & ~(uintptr_t)15);
    *(--sp) = 0;                          /* launch's return address: none */
    *(--sp) = (uint64_t)(uintptr_t)launch;/* context_switch's ret target */
    *(--sp) = 0x202;                      /* rflags: IF set, reserved bit 1 */
    *(--sp) = 0;                          /* rbx */
    *(--sp) = 0;                          /* rbp */
    *(--sp) = 0;                          /* r12 */
    *(--sp) = 0;                          /* r13 */
    *(--sp) = 0;                          /* r14 */
    *(--sp) = 0;                          /* r15  <- RSP points here */
    return (uintptr_t)sp;
}

void arch_cpu_reset(void) {
    /* A triple fault: with an empty IDT the breakpoint cannot be delivered,
       nor can the double fault that follows, and the processor resets. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("cli; lidt %0; int $0x03" : : "m"(null_idt));
    arch_cpu_halt_forever();
}
