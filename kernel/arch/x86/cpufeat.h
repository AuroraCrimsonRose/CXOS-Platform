// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/cpufeat.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The CPU feature table (docs/kernel/CX_KERNEL_PLAN.md section 2.3).
 *
 * CPUID is read once, at boot, into one table, and every optional path asks
 * the table instead of executing CPUID itself. CXOS is built for any x86 CPU,
 * not for the machine it was written on, so a feature is used only where the
 * table says the CPU has it.
 *
 * The reading and the decoding are separate. cpufeat_init executes CPUID and
 * fills a struct cpuid_raw; cpufeat_decode turns that into the table and runs
 * on the host too, so the rules below are tested against register values from
 * CPUs this kernel has never booted on:
 *
 *   - A leaf above the CPU's maximum is never read as data. Intel answers such
 *     a leaf with the highest basic leaf's registers, so leaf 7's bits on an
 *     old CPU would be leaf 2's cache descriptors, read as features.
 *   - Family and model combine the base and extended fields the way the
 *     vendors specify: the extended family only when the base is 0xF, the
 *     extended model only for families 6 and 0xF.
 *   - Features are what the CPU reports. Whether the kernel turned one on (the
 *     OS half of XSAVE, say) is the user's business, not the table's.
 */
#ifndef CXK_ARCH_X86_CPUFEAT_H
#define CXK_ARCH_X86_CPUFEAT_H
#include <stdint.h>

enum cpu_feat {
    /* leaf 1 EDX */
    CPU_FPU, CPU_TSC, CPU_MSR, CPU_PAE, CPU_CX8, CPU_APIC, CPU_PGE, CPU_CMOV,
    CPU_PAT, CPU_CLFSH, CPU_FXSR, CPU_SSE, CPU_SSE2,
    /* leaf 1 ECX */
    CPU_SSE3, CPU_SSSE3, CPU_SSE41, CPU_SSE42, CPU_X2APIC, CPU_POPCNT,
    CPU_XSAVE, CPU_AVX, CPU_RDRAND, CPU_HYPERVISOR,
    /* leaf 7, subleaf 0 */
    CPU_SMEP, CPU_AVX2, CPU_SMAP, CPU_RDSEED, CPU_UMIP,
    /* leaf 1 ECX, kept apart from the rest because it is a paging feature */
    CPU_PCID,
    /* leaf 0x80000001 */
    CPU_NX, CPU_PAGE1G, CPU_RDTSCP, CPU_LM,
    /* leaf 0x80000007 */
    CPU_INVTSC,
    CPU_FEAT_COUNT
};

/* The registers cpufeat_decode needs, as CPUID returned them. A leaf the CPU
   does not have is left zero by the reader, but the decoder does not rely on
   that: it checks the maximum leaves itself. */
struct cpuid_raw {
    uint32_t max_basic;      /* leaf 0 EAX */
    uint32_t vendor[3];      /* leaf 0 EBX, EDX, ECX: the vendor string, in that order */
    uint32_t l1[4];          /* leaf 1: EAX EBX ECX EDX */
    uint32_t l7[4];          /* leaf 7 subleaf 0 */
    uint32_t max_ext;        /* leaf 0x80000000 EAX */
    uint32_t e1[4];          /* leaf 0x80000001 */
    uint32_t e7[4];          /* leaf 0x80000007 */
};

struct cpu_info {
    char     vendor[13];     /* "GenuineIntel", "AuthenticAMD", ...; NUL-terminated */
    uint32_t max_basic, max_ext;
    uint32_t family, model, stepping;
    uint64_t feat;           /* bit (1 << enum cpu_feat) */
};

/* Turn raw registers into the table. Pure: no CPUID, no globals. */
void cpufeat_decode(const struct cpuid_raw *raw, struct cpu_info *out);

/* The short lower-case name of a feature ("sse2"), or "?" out of range. */
const char *cpu_feat_name(enum cpu_feat f);

/* The names of every feature present, space-separated, into buf (always
   NUL-terminated if len > 0). Returns the length written. A name that would
   not fit is left out whole, with "..." to say so. */
uint32_t cpufeat_describe(const struct cpu_info *ci, char *buf, uint32_t len);

#ifndef CXK_HOST_TEST
/* Read CPUID into the table and log it. Once, early in boot. */
void cpufeat_init(void);
/* The table. Zero before cpufeat_init. */
const struct cpu_info *cpu_info(void);
/* Whether the CPU has `f`. Zero for everything before cpufeat_init. */
int cpu_has(enum cpu_feat f);
/* Execute CPUID now (for the self-test that checks the table against it). */
void cpuid_read(uint32_t leaf, uint32_t subleaf, uint32_t r[4]);
#endif

#endif
