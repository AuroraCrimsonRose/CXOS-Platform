// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_madt.c */
/* Aurora Tejeda / CATX Systems */
/* arch/x86/madt.c: which CPUs the MADT says to start (1.9d). */

#include <string.h>
#include "khost.h"
#include "arch/x86/madt.h"

static uint8_t tab[1024];
static uint32_t at;

static void begin(void) { memset(tab, 0, sizeof tab); memcpy(tab, "APIC", 4); at = 44; }

static void lapic(uint8_t uid, uint8_t id, uint32_t flags) {
    uint8_t *e = tab + at;
    e[0] = 0; e[1] = 8; e[2] = uid; e[3] = id;
    e[4] = (uint8_t)flags; e[5] = (uint8_t)(flags >> 8); e[6] = (uint8_t)(flags >> 16); e[7] = (uint8_t)(flags >> 24);
    at += 8;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void x2apic(uint32_t id, uint32_t flags, uint32_t uid) {
    uint8_t *e = tab + at;
    e[0] = 9; e[1] = 16;
    put32(e + 4, id); put32(e + 8, flags); put32(e + 12, uid);
    at += 16;
}

static void ioapic(void) {
    uint8_t *e = tab + at;
    e[0] = 1; e[1] = 12; e[2] = 0x18; put32(e + 4, 0xFEC00000u);
    at += 12;
}

/* The rig's shape (CX_HW_REPORT.md section 7): eight enabled CPUs at
   0x10-0x17, then sixteen disabled entries, the I/O APIC at 0x18. */
KH_TEST(madt_cpus_rig_shape) {
    begin();
    for (uint8_t i = 0; i < 8; i++) lapic((uint8_t)(i + 1), (uint8_t)(0x10 + i), 1);
    for (uint8_t i = 0; i < 16; i++) lapic((uint8_t)(i + 9), (uint8_t)(0x20 + i), 0);
    ioapic();
    struct madt_cpu c[16];
    struct madt_cpu_counts n;
    KH_CHECK_EQ(madt_cpus(tab, at, c, 16, &n), 8u);
    KH_CHECK_EQ(n.enabled, 8u);
    KH_CHECK_EQ(n.disabled, 16u);
    KH_CHECK_EQ(n.malformed, 0u);
    for (uint32_t i = 0; i < 8; i++) {
        KH_CHECK_EQ(c[i].apic_id, 0x10u + i);
        KH_CHECK_EQ(c[i].acpi_uid, i + 1);
    }
}

/* QEMU's shape for `-smp 4,maxcpus=6,sockets=2,cores=3`: a gap at 3, and
   the CPUs that could be hot-added listed disabled. The order is kept. */
KH_TEST(madt_cpus_gaps_and_order) {
    begin();
    lapic(0, 0, 1); lapic(1, 1, 1); lapic(2, 2, 1); lapic(3, 4, 1);
    lapic(4, 5, 0); lapic(5, 6, 0);
    struct madt_cpu c[8];
    struct madt_cpu_counts n;
    KH_CHECK_EQ(madt_cpus(tab, at, c, 8, &n), 4u);
    KH_CHECK_EQ(c[3].apic_id, 4u);
    KH_CHECK_EQ(n.disabled, 2u);
}

/* An x2APIC entry counts as a CPU; one naming an ID a local APIC entry
   already gave is the same CPU; IDs xAPIC IPIs cannot name are not kept;
   the online-capable bit alone (bit 1) is not enabled. */
KH_TEST(madt_cpus_x2apic_duplicates_reach) {
    begin();
    lapic(0, 0, 1);
    x2apic(0, 1, 0);          /* the same CPU again */
    x2apic(7, 1, 7);
    x2apic(0x100, 1, 8);      /* past 0xFE */
    lapic(9, 0xFF, 1);        /* the broadcast ID */
    lapic(10, 9, 2);          /* online capable, not enabled */
    struct madt_cpu c[8];
    struct madt_cpu_counts n;
    KH_CHECK_EQ(madt_cpus(tab, at, c, 8, &n), 2u);
    KH_CHECK_EQ(c[1].apic_id, 7u);
    KH_CHECK_EQ(c[1].acpi_uid, 7u);
    KH_CHECK_EQ(n.duplicate, 1u);
    KH_CHECK_EQ(n.unreachable, 2u);
    KH_CHECK_EQ(n.disabled, 1u);
}

/* More CPUs than room: the first `max` kept, every one counted. A bad
   entry length stops the walk, and what came before it stands. A table
   shorter than its header has none. */
KH_TEST(madt_cpus_cap_and_damage) {
    begin();
    for (uint8_t i = 0; i < 6; i++) lapic(i, i, 1);
    struct madt_cpu c[4];
    struct madt_cpu_counts n;
    KH_CHECK_EQ(madt_cpus(tab, at, c, 4, &n), 4u);
    KH_CHECK_EQ(n.enabled, 6u);

    tab[at] = 0; tab[at + 1] = 0;          /* length 0 */
    KH_CHECK_EQ(madt_cpus(tab, at + 8, c, 4, &n), 4u);
    KH_CHECK_EQ(n.malformed, 1u);

    begin();
    lapic(0, 0, 1);
    tab[at] = 0; tab[at + 1] = 8;          /* runs past the end */
    KH_CHECK_EQ(madt_cpus(tab, at + 4, c, 4, &n), 1u);
    KH_CHECK_EQ(n.malformed, 1u);

    KH_CHECK_EQ(madt_cpus(tab, 40, c, 4, &n), 0u);
}
