// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/idt.h */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 IDT: the 32 CPU exceptions (isr.asm), the double fault on its
   IST stack, the 16 legacy IRQs at 32-47, the MSI pool above them, and the
   local APIC's spurious vector. arch/irq.h is implemented in idt.c. */

#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>
#include "arch/irq.h"

/* What isr.asm leaves on the stack, lowest address first. */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;      /* pushed by the CPU */
};

/* The vectors past the exceptions, as on x86-32: legacy IRQ n at 32 + n, the
   MSI pool (arch/irq.h) above them, and the spurious vector the local APIC is
   given (arch/x86/apic.c). */
#define IRQ_VECTOR_BASE   32
#define MSI_VECTOR_BASE   48
#define MSI_VECTOR_COUNT  8
#define SPURIOUS_VECTOR64 0xFF

void idt64_init(void);

/* A self-test arms a probe before it raises an exception on purpose. The probe
   sees every exception first and returns 1 if it dealt with it (the frame,
   changed or not, is then resumed) or 0 to let it panic. One at a time; NULL
   disarms. */
typedef int (*trap_probe_fn)(struct registers *frame);
void idt64_set_probe(trap_probe_fn probe);

/* For the self-tests: the IDT's base, and gate n's IST index. */
uintptr_t idt64_base(void);
unsigned  idt64_gate_ist(unsigned vector);

#endif
