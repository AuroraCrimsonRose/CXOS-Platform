// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/ktest_arch.c */
/* Aurora Tejeda / CATX Systems */
/* arch/ktest_arch.h on 32-bit x86: the boot self-tests that read the CPU's
   own tables, kept here because no file outside kernel/arch/ may. */

#include <stdint.h>
#include "arch/ktest_arch.h"
#include "arch/x86/apic.h"
#include "arch/x86/cpufeat.h"
#include "gdt.h"
#include "idt.h"
#include "boot/cxbi.h"
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

/* ---- the CPU feature table is this CPU's (roadmap step 3a) ----
 * The decoding rules are host-tested (test_cpufeat.c). This proves the table
 * the kernel booted with was read from the CPU it is running on: rebuilt here
 * from CPUID executed now, leaf by leaf, it must match exactly. A table left
 * empty, read from the wrong leaf, or filled before the CPU was asked fails.
 * And the APIC code must have obeyed it: no local APIC in the table means
 * none was brought up. Booted on a minimal and a full CPU model, both sides
 * of that run. */
int ktest_arch_cpu_features(void) {
    const struct cpu_info *ci = cpu_info();
    struct cpuid_raw raw = { 0 };
    uint32_t r[4];
    cpuid_read(0, 0, r);
    raw.max_basic = r[0];
    raw.vendor[0] = r[1]; raw.vendor[1] = r[3]; raw.vendor[2] = r[2];
    if (raw.max_basic >= 1) cpuid_read(1, 0, raw.l1);
    if (raw.max_basic >= 7) cpuid_read(7, 0, raw.l7);
    cpuid_read(0x80000000u, 0, r);
    raw.max_ext = r[0];
    if ((raw.max_ext >> 16) == 0x8000u && raw.max_ext >= 0x80000001u) cpuid_read(0x80000001u, 0, raw.e1);
    if ((raw.max_ext >> 16) == 0x8000u && raw.max_ext >= 0x80000007u) cpuid_read(0x80000007u, 0, raw.e7);
    struct cpu_info now;
    cpufeat_decode(&raw, &now);

    if (ci->max_basic < 1 || ci->vendor[0] == 0) return 0;   /* never filled */
    for (int i = 0; i < 13; i++) if (ci->vendor[i] != now.vendor[i]) return 0;
    if (ci->feat != now.feat || ci->family != now.family || ci->model != now.model) return 0;
    if (cpu_has(CPU_FPU) != (int)((raw.l1[3] >> 0) & 1u)) return 0;   /* cpu_has reads it */
    if (lapic_active() && !cpu_has(CPU_APIC)) return 0;   /* APIC obeyed the table */
    /* The loader read CPUID too and handed it over (boot/cxbi.h); it saw this
       CPU. Leaf 1 bits the kernel itself changes are left out: ECX 27 is
       OSXSAVE (CR4), EDX 9 the local APIC's global enable (IA32_APIC_BASE). */
    const struct cxk_boot_info *b = cxbi();
    if (b->cpuid_max_leaf != raw.max_basic) return 0;
    if ((b->cpuid1_ecx & ~(1u << 27)) != (raw.l1[2] & ~(1u << 27))) return 0;
    if ((b->cpuid1_edx & ~(1u << 9)) != (raw.l1[3] & ~(1u << 9))) return 0;
    return 1;
}
