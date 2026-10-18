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
