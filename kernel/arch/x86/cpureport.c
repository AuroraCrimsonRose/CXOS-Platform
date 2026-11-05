// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/cpureport.c */
/* Aurora Tejeda / CATX Systems */
/* arch/hwreport.h on x86: the CPU's identity, its raw CPUID leaves, the
   hypervisor it runs under, and the local and I/O APICs' state.
     cpu <vendor> <family> <model> <stepping> <brand>
     cpuid <leaf> <subleaf> <eax> <ebx> <ecx> <edx>
     hypervisor <signature> <max-leaf> | hypervisor -
     apic <io-apic-routing 0|1> <local-apic 0|1> <this-cpu's-id> */

#include "arch/hwreport.h"
#include "arch/x86/apic.h"
#include "arch/x86/cpufeat.h"

static void cpuid_line(struct hw_out *o, uint32_t leaf, uint32_t sub) {
    uint32_t r[4];
    cpuid_read(leaf, sub, r);
    hw_line(o, "cpuid");
    hw_x(o, leaf, 8);
    hw_x(o, sub, 2);
    for (int i = 0; i < 4; i++) hw_x(o, r[i], 8);
    hw_done(o);
}

void arch_hw_report(struct hw_out *o) {
    const struct cpu_info *ci = cpu_info();
    uint32_t brand[12] = { 0 };
    if (ci->max_ext >= 0x80000004u)
        for (uint32_t i = 0; i < 3; i++) cpuid_read(0x80000002u + i, 0, brand + 4 * i);
    hw_line(o, "cpu");
    hw_sn(o, ci->vendor, 12);
    hw_x(o, ci->family, 2);
    hw_x(o, ci->model, 2);
    hw_x(o, ci->stepping, 1);
    hw_sn(o, (const char *)brand, 48);
    hw_done(o);

    /* Raw, not decoded: two machines are compared bit for bit, and a bit the
       decoder does not know still differs. */
    static const uint32_t basic[][2] = { {0, 0}, {1, 0}, {6, 0}, {7, 0} };
    static const uint32_t ext[] = { 0x80000000u, 0x80000001u, 0x80000007u, 0x80000008u };
    for (unsigned i = 0; i < sizeof basic / sizeof basic[0]; i++)
        if (basic[i][0] <= ci->max_basic) cpuid_line(o, basic[i][0], basic[i][1]);
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (ext[i] <= ci->max_ext) cpuid_line(o, ext[i], 0);

    uint32_t l1[4];
    cpuid_read(1, 0, l1);
    hw_line(o, "hypervisor");
    if (l1[2] & (1u << 31)) {
        uint32_t h[4];
        cpuid_read(0x40000000u, 0, h);
        hw_sn(o, (const char *)&h[1], 12);
        hw_x(o, h[0], 8);
        hw_done(o);
        cpuid_line(o, 0x40000000u, 0);
        if (h[0] >= 0x40000001u) cpuid_line(o, 0x40000001u, 0);
    } else {
        hw_s(o, "-");
        hw_done(o);
    }

    hw_line(o, "apic");
    hw_u(o, (uint64_t)apic_active());
    hw_u(o, (uint64_t)lapic_active());
    hw_x(o, lapic_id(), 8);
    hw_done(o);
}
