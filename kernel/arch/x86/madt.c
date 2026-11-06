// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/madt.c */
/* Aurora Tejeda / CATX Systems */
/* The MADT's processor list (madt.h). */

#include "madt.h"

#define MADT_HEAD    44u        /* the SDT header, the local APIC address, flags */
#define MADT_LAPIC   0x0u       /* acpi id u8, apic id u8, flags u32: 8 bytes */
#define MADT_X2APIC  0x9u       /* reserved u16, x2apic id u32, flags u32, uid u32: 16 */
#define XAPIC_BROADCAST 0xFFu   /* never a CPU's ID */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t madt_cpus(const uint8_t *m, uint32_t len,
                   struct madt_cpu *out, uint32_t max, struct madt_cpu_counts *c) {
    struct madt_cpu_counts n = {0};
    uint32_t kept = 0;

    for (uint32_t off = MADT_HEAD; len >= MADT_HEAD && off < len; ) {
        if (len - off < 2) { n.malformed++; break; }
        const uint8_t *e = m + off;
        uint32_t el = e[1];
        if (el < 2 || el > len - off) { n.malformed++; break; }
        off += el;

        uint32_t id, uid, flags;
        if (e[0] == MADT_LAPIC && el >= 8) {
            uid = e[2]; id = e[3]; flags = rd32(e + 4);
        } else if (e[0] == MADT_X2APIC && el >= 16) {
            id = rd32(e + 4); flags = rd32(e + 8); uid = rd32(e + 12);
        } else {
            continue;
        }

        if (!(flags & MADT_CPU_ENABLED)) { n.disabled++; continue; }
        if (id >= XAPIC_BROADCAST)       { n.unreachable++; continue; }
        int seen = 0;
        for (uint32_t i = 0; i < kept; i++)
            if (out[i].apic_id == id) seen = 1;
        if (seen) { n.duplicate++; continue; }

        n.enabled++;
        if (kept < max) { out[kept].apic_id = id; out[kept].acpi_uid = uid; kept++; }
    }
    if (c) *c = n;
    return kept;
}
