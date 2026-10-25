// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/gdt.h */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 GDT and TSS. Ring 0 only for now: the user segments come with
   syscall/sysret (roadmap step 5e), whose STAR layout fixes their order. */

#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT64_KCODE   0x08
#define GDT64_KDATA   0x10
#define GDT64_TSS     0x18      /* a 16-byte descriptor: slots 3 and 4 */

/* The IST slot the double fault runs on. The 32-bit kernel gives it a task
   gate and a TSS of its own; on x86-64 an IST stack is the same guarantee, a
   good stack whatever state RSP was in when the fault hit. */
#define IST_DOUBLE_FAULT  1

void gdt64_init(void);

/* For the bring-up self-tests: where the GDT is, the TSS descriptor's low
   quadword (its type says whether the TSS is busy, so whether ltr ran), and
   the double-fault stack's bounds. */
uintptr_t gdt64_base(void);
uint64_t  gdt64_tss_descriptor(void);
void      gdt64_df_stack(uintptr_t *lo, uintptr_t *hi);

/* RSP0, the stack an interrupt from ring 3 switches to (cpu.c sets it on
   every thread switch), and the dedicated one used while the running thread
   has none. */
void      gdt64_set_rsp0(uintptr_t top);
uintptr_t gdt64_rsp0(void);
uintptr_t gdt64_default_rsp0(void);

#endif
