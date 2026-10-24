// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/idt.h */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 IDT: the 32 CPU exceptions (isr.asm), the double fault on its
   IST stack. Device interrupts and the APIC come in roadmap step 5d. */

#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>

/* What isr.asm leaves on the stack, lowest address first. */
struct trap_frame64 {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;      /* pushed by the CPU */
};

void idt64_init(void);

/* A self-test arms a probe before it raises an exception on purpose. The probe
   sees every exception first and returns 1 if it dealt with it (the frame,
   changed or not, is then resumed) or 0 to let it panic. One at a time; NULL
   disarms. */
typedef int (*trap_probe_fn)(struct trap_frame64 *frame);
void idt64_set_probe(trap_probe_fn probe);

/* For the self-tests: the IDT's base, and gate n's IST index. */
uintptr_t idt64_base(void);
unsigned  idt64_gate_ist(unsigned vector);

#endif
