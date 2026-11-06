// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/madt.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The processors the MADT lists (1.9d): which local APIC IDs to start.
 *
 * The MADT is the only list of CPUs a kernel can trust. CPUID leaf 1's
 * initial APIC ID is the ID the CPU had at reset, and on the rig it says 0
 * for a BSP whose local APIC answers 0x10 (CX_HW_REPORT.md section 7), so an
 * INIT aimed by it would land on nothing. Counting entries is wrong too: the
 * rig's MADT has 24, of which 16 are disabled, and QEMU's lists the CPUs
 * that could be hot-added the same way. So a CPU is started only if its
 * entry says enabled, and by the ID that entry names. Haiku's loader reads
 * the table the same way (smp_do_acpi_config, boot/platform/bios_ia32/
 * smp.cpp: the enabled flag, then apic_id).
 *
 * Pure code, no hardware: the host tests build it (kernel/test/host).
 */
#ifndef ARCH_X86_MADT_H
#define ARCH_X86_MADT_H

#include <stdint.h>

#define MADT_CPU_ENABLED  0x1u   /* flags bit 0: usable now */

struct madt_cpu {
    uint32_t apic_id;            /* what an IPI names it by */
    uint32_t acpi_uid;           /* the processor's ACPI id */
};

/* What a walk found besides the CPUs it kept. */
struct madt_cpu_counts {
    uint32_t enabled;            /* kept (or would have been: past `max`) */
    uint32_t disabled;           /* flags bit 0 clear: never started */
    uint32_t duplicate;          /* an ID already kept (a local APIC entry and
                                    an x2APIC one for the same CPU) */
    uint32_t unreachable;        /* an ID past 0xFE: xAPIC IPIs cannot name it */
    uint32_t malformed;          /* the walk stopped at a bad entry length */
};

/* Walk the MADT (`len` bytes, the header included) and keep each enabled
   processor's ID, local APIC (type 0) and x2APIC (type 9) entries alike, in
   table order, up to `max`. Returns how many were kept. */
uint32_t madt_cpus(const uint8_t *madt, uint32_t len,
                   struct madt_cpu *out, uint32_t max, struct madt_cpu_counts *counts);

#endif
