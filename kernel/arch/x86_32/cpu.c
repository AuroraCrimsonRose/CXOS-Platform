// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/cpu.c */
/* Aurora Tejeda / CATX Systems */
/* arch/cpu.h on 32-bit x86: the GDT, the TSS and the IDT. */

#include "arch/cpu.h"
#include "gdt.h"
#include "idt.h"
#include "logging.h"

void arch_cpu_init(void) {
    gdt_init();
    klog("GDT", SEV_OK, "GDT + TSS installed");
    idt_init();
    klog("IDT", SEV_OK, "installed");
}

void arch_kernel_stack_set(uintptr_t top) { tss_set_kernel_stack((uint32_t)top); }

uintptr_t arch_kernel_stack_default(void) { return tss_default_kernel_stack(); }
