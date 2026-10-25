// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/ktest_x86.c */
/* Aurora Tejeda / CATX Systems */
/* arch/ktest_arch.h, the part both x86 kernels share: the CPU feature table
   and the APIC code that obeys it are arch/x86's, on either width. */

#include <stdint.h>
#include "arch/ktest_arch.h"
#include "arch/x86/apic.h"
#include "arch/x86/cpufeat.h"
#include "boot/cxbi.h"

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
