// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/cpu.c */
/* Aurora Tejeda / CATX Systems */
/* arch/cpu.h and arch/usermode.h on 32-bit x86: the GDT, the TSS, the IDT,
   the feature table, and the int 0x80 gate. */

#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/usermode.h"
#include "arch/x86/cpufeat.h"
#include "gdt.h"
#include "idt.h"
#include "logging.h"

void arch_cpu_init(void) {
    gdt_init();
    klog("GDT", SEV_OK, "GDT + TSS installed");
    idt_init();
    klog("IDT", SEV_OK, "installed");
    /* Before anything that might ask what the CPU has. */
    cpufeat_init();
}

void arch_kernel_stack_set(uintptr_t top) { tss_set_kernel_stack((uint32_t)top); }

uintptr_t arch_kernel_stack_default(void) { return tss_default_kernel_stack(); }

uintptr_t arch_kernel_stack_current(void) { return tss_current_esp0(); }

void arch_run_on_stack(uintptr_t top, void (*fn)(void)) {
    /* Fixed registers, not "r": the compiler may not hand out esp, but it may
       hand out ebp, which the xor clears before the call reads it. */
    __asm__ volatile ("mov %0, %%esp\n\t"
                      "xor %%ebp, %%ebp\n\t"      /* ends a panic's stack trace here */
                      "call *%1"
                      : : "a"(top), "c"(fn) : "memory");
    __builtin_unreachable();
}

uintptr_t arch_thread_frame(uintptr_t stack_top, void (*launch)(void)) {
    /* context_switch (switch.asm) saves pushfd, ebx, esi, edi, ebp, so from
       the saved esp upward the frame is [ebp][edi][esi][ebx][eflags][return],
       and it restores with pop ebp; pop edi; pop esi; pop ebx; popfd; ret. */
    uint32_t *sp = (uint32_t *)stack_top;
    *(--sp) = (uint32_t)launch;          /* [+20] ret target */
    *(--sp) = 0x202;                     /* [+16] eflags: IF set, reserved bit 1 */
    *(--sp) = 0;                         /* [+12] ebx */
    *(--sp) = 0;                         /* [+8]  esi */
    *(--sp) = 0;                         /* [+4]  edi */
    *(--sp) = 0;                         /* [+0]  ebp  <- esp points here */
    return (uintptr_t)sp;
}

void arch_cpu_reset(void) {
    /* A triple fault: with an empty IDT the breakpoint cannot be delivered,
       nor can the double fault that follows, and the processor resets. */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("cli; lidt %0; int $0x03" : : "m"(null_idt));
    arch_cpu_halt_forever();
}

/* usermode.asm */
extern void syscall_stub(void);

void arch_syscall_gate_init(void) {
    /* int 0x80, DPL 3 so ring 3 can invoke it */
    idt_set_user_gate(0x80, (uint32_t)syscall_stub);
}

void arch_syscall_stub(uint8_t *dst, uint32_t nr) {
    dst[0] = 0xB8;                               /* mov eax, nr */
    dst[1] = (uint8_t)nr;         dst[2] = (uint8_t)(nr >> 8);
    dst[3] = (uint8_t)(nr >> 16); dst[4] = (uint8_t)(nr >> 24);
    dst[5] = 0xCD; dst[6] = 0x80;                /* int 0x80 */
    dst[7] = 0xC3;                               /* ret */
}

uint32_t arch_syscall_stub_ret(void) { return 7; }   /* the ret */
