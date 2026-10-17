// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_cpufeat.c */
/* Aurora Tejeda / CATX Systems */
/* The CPU feature table's decoder (arch/x86/cpufeat.h), on register values a
   boot in QEMU never produces: other vendors, old CPUs, leaves past the
   maximum. */
#include "khost.h"
#include "arch/x86/cpufeat.h"
#include <string.h>

#define HAS(ci, f) (((ci).feat >> (f)) & 1u)

/* "AuthenticAMD" as CPUID leaf 0 returns it: EBX, EDX, ECX. */
static void amd(struct cpuid_raw *r) {
    r->vendor[0] = 0x68747541u;   /* "Auth" */
    r->vendor[1] = 0x69746E65u;   /* "enti" */
    r->vendor[2] = 0x444D4163u;   /* "cAMD" */
}

/* The target rig: an FX-8350 (Piledriver), family 15h. */
KH_TEST(cpufeat_fx8350) {
    struct cpuid_raw r = { 0 };
    struct cpu_info ci;
    amd(&r);
    r.max_basic = 0xD;
    r.l1[0] = 0x00600F20u;                          /* family F + 6, model 2, stepping 0 */
    r.l1[3] = (1u << 0) | (1u << 9) | (1u << 25) | (1u << 26);   /* fpu apic sse sse2 */
    r.l1[2] = (1u << 0) | (1u << 26) | (1u << 28);              /* sse3 xsave avx */
    r.l7[1] = (1u << 3);                            /* bmi1 only: no smep, no avx2 */
    r.max_ext = 0x8000001Eu;
    r.e1[3] = (1u << 20) | (1u << 26) | (1u << 27) | (1u << 29); /* nx page1g rdtscp lm */
    r.e7[3] = (1u << 8);                            /* invariant tsc */
    cpufeat_decode(&r, &ci);
    KH_CHECK(strcmp(ci.vendor, "AuthenticAMD") == 0);
    KH_CHECK_EQ(ci.family, 0x15);
    KH_CHECK_EQ(ci.model, 2);
    KH_CHECK_EQ(ci.stepping, 0);
    KH_CHECK(HAS(ci, CPU_FPU) && HAS(ci, CPU_APIC) && HAS(ci, CPU_SSE) && HAS(ci, CPU_SSE2));
    KH_CHECK(HAS(ci, CPU_SSE3) && HAS(ci, CPU_XSAVE) && HAS(ci, CPU_AVX));
    KH_CHECK(!HAS(ci, CPU_SMEP) && !HAS(ci, CPU_AVX2) && !HAS(ci, CPU_SMAP));
    KH_CHECK(HAS(ci, CPU_NX) && HAS(ci, CPU_PAGE1G) && HAS(ci, CPU_RDTSCP) && HAS(ci, CPU_LM));
    KH_CHECK(HAS(ci, CPU_INVTSC));
    KH_CHECK(!HAS(ci, CPU_HYPERVISOR));
    KH_CHECK_EQ(ci.max_ext, 0x8000001Eu);
}

/* Family 6 takes the extended model: a Skylake is model 0x5E, not 0xE. */
KH_TEST(cpufeat_family6_extended_model) {
    struct cpuid_raw r = { 0 };
    struct cpu_info ci;
    r.max_basic = 0x16;
    r.l1[0] = 0x000506E3u;
    cpufeat_decode(&r, &ci);
    KH_CHECK_EQ(ci.family, 6);
    KH_CHECK_EQ(ci.model, 0x5E);
    KH_CHECK_EQ(ci.stepping, 3);
}

/* The extended fields only count where the vendors say: a family-5 CPU's
   extended model bits are not part of its model, and the extended family
   only adds to a base family of 0xF. */
KH_TEST(cpufeat_extended_fields_only_where_defined) {
    struct cpuid_raw r = { 0 };
    struct cpu_info ci;
    r.max_basic = 1;
    r.l1[0] = 0x0FFF0543u;          /* ext family FF, ext model F, family 5, model 4 */
    cpufeat_decode(&r, &ci);
    KH_CHECK_EQ(ci.family, 5);
    KH_CHECK_EQ(ci.model, 4);
    r.l1[0] = 0x00010F21u;          /* family F, ext family 0, ext model 1 */
    cpufeat_decode(&r, &ci);
    KH_CHECK_EQ(ci.family, 0xF);
    KH_CHECK_EQ(ci.model, 0x12);
}

/* A leaf above the maximum is never read as data. Intel answers one with the
   highest basic leaf's registers, so on an old CPU "leaf 7" is cache
   descriptors; here it is every bit set, and none of it may show. */
KH_TEST(cpufeat_leaf_above_max_is_ignored) {
    struct cpuid_raw r;
    struct cpu_info ci;
    memset(&r, 0xFF, sizeof r);
    r.max_basic = 2;
    r.max_ext   = 0x00000002u;      /* no extended leaves: not 0x8000xxxx */
    cpufeat_decode(&r, &ci);
    KH_CHECK(HAS(ci, CPU_SSE2));                 /* leaf 1 is real */
    KH_CHECK(!HAS(ci, CPU_SMEP) && !HAS(ci, CPU_UMIP) && !HAS(ci, CPU_RDSEED));
    KH_CHECK(!HAS(ci, CPU_NX) && !HAS(ci, CPU_LM) && !HAS(ci, CPU_INVTSC));
    KH_CHECK_EQ(ci.max_ext, 0);

    /* Nor one that is merely large: every bit set is past every real leaf
       number, and still not an extended maximum. */
    r.max_ext = 0xFFFFFFFFu;
    cpufeat_decode(&r, &ci);
    KH_CHECK(!HAS(ci, CPU_NX) && !HAS(ci, CPU_LM) && !HAS(ci, CPU_INVTSC));
    KH_CHECK_EQ(ci.max_ext, 0);

    /* Extended leaves up to 0x80000001 only: 0x80000007 is past them. */
    r.max_ext = 0x80000001u;
    cpufeat_decode(&r, &ci);
    KH_CHECK(HAS(ci, CPU_NX) && HAS(ci, CPU_LM));
    KH_CHECK(!HAS(ci, CPU_INVTSC));

    /* Not even leaf 1. */
    r.max_basic = 0;
    cpufeat_decode(&r, &ci);
    KH_CHECK(!HAS(ci, CPU_FPU) && !HAS(ci, CPU_SSE2));
    KH_CHECK_EQ(ci.family, 0);
}

/* Every register bit names at most one feature, and the ones the kernel
   leans on are where the manuals put them. */
KH_TEST(cpufeat_bits_are_where_documented) {
    static const struct { int leaf, reg, bit; enum cpu_feat f; } known[] = {
        { 1, 3,  0, CPU_FPU },  { 1, 3,  9, CPU_APIC }, { 1, 3, 24, CPU_FXSR },
        { 1, 3, 25, CPU_SSE },  { 1, 3, 26, CPU_SSE2 }, { 1, 3, 13, CPU_PGE },
        { 1, 2, 17, CPU_PCID }, { 1, 2, 21, CPU_X2APIC }, { 1, 2, 30, CPU_RDRAND },
        { 1, 2, 31, CPU_HYPERVISOR },
        { 7, 1,  7, CPU_SMEP }, { 7, 1, 20, CPU_SMAP }, { 7, 2,  2, CPU_UMIP },
        { 7, 1, 18, CPU_RDSEED },
        { 0x81, 3, 20, CPU_NX }, { 0x81, 3, 29, CPU_LM }, { 0x87, 3, 8, CPU_INVTSC },
    };
    for (unsigned k = 0; k < sizeof known / sizeof known[0]; k++) {
        struct cpuid_raw r = { 0 };
        struct cpu_info ci;
        r.max_basic = 7;
        r.max_ext = 0x80000008u;
        uint32_t *regs = known[k].leaf == 1 ? r.l1 : known[k].leaf == 7 ? r.l7
                       : known[k].leaf == 0x81 ? r.e1 : r.e7;
        regs[known[k].reg] = 1u << known[k].bit;
        cpufeat_decode(&r, &ci);
        KH_CHECK_EQ(ci.feat, 1ull << known[k].f);   /* that one, and nothing else */
    }

    for (int set = 0; set < 4; set++)
        for (int reg = 0; reg < 4; reg++)
            for (int bit = 0; bit < 32; bit++) {
                struct cpuid_raw r = { 0 };
                struct cpu_info ci;
                r.max_basic = 7;
                r.max_ext = 0x80000008u;
                uint32_t *regs = set == 0 ? r.l1 : set == 1 ? r.l7 : set == 2 ? r.e1 : r.e7;
                if (set == 0 && reg == 0) continue;     /* leaf 1 EAX is the signature */
                regs[reg] = 1u << bit;
                cpufeat_decode(&r, &ci);
                KH_CHECK((ci.feat & (ci.feat - 1)) == 0);   /* zero or one feature */
            }
}

KH_TEST(cpufeat_vendor_is_printable) {
    struct cpuid_raw r = { 0 };
    struct cpu_info ci;
    r.vendor[0] = 0x756E6547u;   /* "Genu" */
    r.vendor[1] = 0x49656E69u;   /* "ineI" */
    r.vendor[2] = 0x006C6574u;   /* "tel" and a NUL, where "ntel" belongs */
    cpufeat_decode(&r, &ci);
    KH_CHECK(strcmp(ci.vendor, "GenuineItel?") == 0);    /* the NUL shows as ?, not as an end */
    KH_CHECK_EQ(ci.vendor[12], 0);
}

KH_TEST(cpufeat_describe_names_and_truncates) {
    struct cpu_info ci;
    memset(&ci, 0, sizeof ci);
    ci.feat = (1ull << CPU_FPU) | (1ull << CPU_TSC) | (1ull << CPU_SSE2);
    char buf[64];
    KH_CHECK_EQ(cpufeat_describe(&ci, buf, sizeof buf), 12);
    KH_CHECK(strcmp(buf, "fpu tsc sse2") == 0);

    char small[8];
    memset(small, 'x', sizeof small);
    KH_CHECK_EQ(cpufeat_describe(&ci, small, sizeof small), 7);
    KH_CHECK(strcmp(small, "fpu ...") == 0);

    char tiny[3];
    memset(tiny, 'x', sizeof tiny);
    cpufeat_describe(&ci, tiny, sizeof tiny);
    KH_CHECK_EQ(tiny[2], 0);                     /* terminated inside the buffer */

    KH_CHECK_EQ(cpufeat_describe(&ci, buf, 0), 0);

    ci.feat = 0;
    KH_CHECK_EQ(cpufeat_describe(&ci, buf, sizeof buf), 0);
    KH_CHECK_EQ(buf[0], 0);

    KH_CHECK(strcmp(cpu_feat_name(CPU_SSE41), "sse4.1") == 0);
    KH_CHECK(strcmp(cpu_feat_name(CPU_FEAT_COUNT), "?") == 0);
}
