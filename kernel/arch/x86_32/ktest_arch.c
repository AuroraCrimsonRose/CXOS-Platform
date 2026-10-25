// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/ktest_arch.c */
/* Aurora Tejeda / CATX Systems */
/* arch/ktest_arch.h on 32-bit x86: the boot self-tests that read the CPU's
   own tables, kept here because no file outside kernel/arch/ may. */

#include <stdint.h>
#include "arch/ktest_arch.h"
#include "gdt.h"
#include "idt.h"
#include "usermode.h"            /* usermode_run_blob */

/* ---- the double fault has a stack of its own ----
 * Checks the wiring rather than taking a double fault, which cannot be
 * recovered from: vector 8 is a task gate, and it names an available 32-bit
 * TSS. (CXK_KTEST_STACK_OVERFLOW takes the real one.) */
int ktest_arch_double_fault_stack(void) {
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idtr, gdtr;
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    const struct idt_entry *e = (const struct idt_entry *)idtr.base + 8;
    if (e->type_attr != IDT_GATE_TASK || e->selector != DF_TSS_SEL) return 0;
    const uint8_t *d = (const uint8_t *)gdtr.base + DF_TSS_SEL;
    return d[5] == 0x89;   /* present, DPL 0, 32-bit TSS, not busy */
}

/* ---- a syscall leaves ring 3 its segment registers ----
 * syscall_stub loads the kernel's data selector and used to iretd without
 * giving the caller's back. The CPU then nulls DS, ES, FS and GS on the way
 * to ring 3, and the program's next DS access is #GP(0): pids 2 and 3 on
 * the FX. QEMU nulls the selector but keeps the base and does not check it,
 * so programs ran there - but the selector value it shows is null, so a
 * ring-3 routine that reads its selectors after a syscall sees the bug. */
extern uint8_t user_blob_segs_start[];
extern uint8_t user_blob_segs_end[];

int ktest_arch_user_segments_kept(void) {
    int rc = usermode_run_blob(user_blob_segs_start,
                               (uint32_t)(user_blob_segs_end - user_blob_segs_start));
    const int want = USER_DATA_SEL | USER_DATA_SEL << 8 | USER_DATA_SEL << 16 | USER_DATA_SEL << 24;
    return rc == want;
}

/* None beyond the three above. */
const struct ktest_arch_case *ktest_arch_cases(unsigned *count) {
    *count = 0;
    return 0;
}
