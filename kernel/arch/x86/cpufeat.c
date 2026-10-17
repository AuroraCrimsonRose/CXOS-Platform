// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/cpufeat.c */
/* Aurora Tejeda / CATX Systems */
/* The CPU feature table: see cpufeat.h. */
#include "arch/x86/cpufeat.h"

enum { R_EAX, R_EBX, R_ECX, R_EDX };
enum { S_L1, S_L7, S_E1, S_E7 };

/* Where each feature's bit lives. One row per feature, in enum order, so a
   feature added to the enum without a row here fails the _Static_assert. */
static const struct { uint8_t src, reg, bit; const char *name; } where[] = {
    [CPU_FPU]        = { S_L1, R_EDX,  0, "fpu" },
    [CPU_TSC]        = { S_L1, R_EDX,  4, "tsc" },
    [CPU_MSR]        = { S_L1, R_EDX,  5, "msr" },
    [CPU_PAE]        = { S_L1, R_EDX,  6, "pae" },
    [CPU_CX8]        = { S_L1, R_EDX,  8, "cx8" },
    [CPU_APIC]       = { S_L1, R_EDX,  9, "apic" },
    [CPU_PGE]        = { S_L1, R_EDX, 13, "pge" },
    [CPU_CMOV]       = { S_L1, R_EDX, 15, "cmov" },
    [CPU_PAT]        = { S_L1, R_EDX, 16, "pat" },
    [CPU_CLFSH]      = { S_L1, R_EDX, 19, "clflush" },
    [CPU_FXSR]       = { S_L1, R_EDX, 24, "fxsr" },
    [CPU_SSE]        = { S_L1, R_EDX, 25, "sse" },
    [CPU_SSE2]       = { S_L1, R_EDX, 26, "sse2" },
    [CPU_SSE3]       = { S_L1, R_ECX,  0, "sse3" },
    [CPU_SSSE3]      = { S_L1, R_ECX,  9, "ssse3" },
    [CPU_SSE41]      = { S_L1, R_ECX, 19, "sse4.1" },
    [CPU_SSE42]      = { S_L1, R_ECX, 20, "sse4.2" },
    [CPU_X2APIC]     = { S_L1, R_ECX, 21, "x2apic" },
    [CPU_POPCNT]     = { S_L1, R_ECX, 23, "popcnt" },
    [CPU_XSAVE]      = { S_L1, R_ECX, 26, "xsave" },
    [CPU_AVX]        = { S_L1, R_ECX, 28, "avx" },
    [CPU_RDRAND]     = { S_L1, R_ECX, 30, "rdrand" },
    [CPU_HYPERVISOR] = { S_L1, R_ECX, 31, "hypervisor" },
    [CPU_SMEP]       = { S_L7, R_EBX,  7, "smep" },
    [CPU_AVX2]       = { S_L7, R_EBX,  5, "avx2" },
    [CPU_SMAP]       = { S_L7, R_EBX, 20, "smap" },
    [CPU_RDSEED]     = { S_L7, R_EBX, 18, "rdseed" },
    [CPU_UMIP]       = { S_L7, R_ECX,  2, "umip" },
    [CPU_PCID]       = { S_L1, R_ECX, 17, "pcid" },
    [CPU_NX]         = { S_E1, R_EDX, 20, "nx" },
    [CPU_PAGE1G]     = { S_E1, R_EDX, 26, "page1g" },
    [CPU_RDTSCP]     = { S_E1, R_EDX, 27, "rdtscp" },
    [CPU_LM]         = { S_E1, R_EDX, 29, "lm" },
    [CPU_INVTSC]     = { S_E7, R_EDX,  8, "invtsc" },
};
_Static_assert(sizeof where / sizeof where[0] == CPU_FEAT_COUNT, "a feature has no row");
_Static_assert(CPU_FEAT_COUNT <= 64, "the table is one uint64_t");

/* An extended leaf exists only if the CPU says so. A CPU without extended
   leaves answers 0x80000000 with something that is not 0x8000xxxx (Intel
   repeats its highest basic leaf), so the top half is checked too. */
static int ext_has(uint32_t max_ext, uint32_t leaf) {
    return (max_ext & 0xFFFF0000u) == 0x80000000u && max_ext >= leaf;
}

void cpufeat_decode(const struct cpuid_raw *raw, struct cpu_info *out) {
    for (uint32_t i = 0; i < sizeof out->vendor; i++) out->vendor[i] = 0;
    out->max_basic = raw->max_basic;
    out->max_ext   = ext_has(raw->max_ext, 0x80000000u) ? raw->max_ext : 0;
    out->family = out->model = out->stepping = 0;
    out->feat = 0;

    for (int w = 0; w < 3; w++)
        for (int b = 0; b < 4; b++) {
            char c = (char)(raw->vendor[w] >> (8 * b));
            out->vendor[w * 4 + b] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }

    const uint32_t *src[4] = {
        raw->max_basic >= 1                  ? raw->l1 : 0,
        raw->max_basic >= 7                  ? raw->l7 : 0,
        ext_has(raw->max_ext, 0x80000001u)   ? raw->e1 : 0,
        ext_has(raw->max_ext, 0x80000007u)   ? raw->e7 : 0,
    };

    if (src[S_L1]) {
        uint32_t a = raw->l1[R_EAX];
        uint32_t fam = (a >> 8) & 0xFu, mod = (a >> 4) & 0xFu;
        if (fam == 0xFu)              fam += (a >> 20) & 0xFFu;
        if (fam == 6u || fam >= 0xFu) mod += ((a >> 16) & 0xFu) << 4;
        out->family   = fam;
        out->model    = mod;
        out->stepping = a & 0xFu;
    }

    for (uint32_t f = 0; f < CPU_FEAT_COUNT; f++) {
        const uint32_t *r = src[where[f].src];
        if (r && (r[where[f].reg] >> where[f].bit) & 1u) out->feat |= 1ull << f;
    }
}

const char *cpu_feat_name(enum cpu_feat f) {
    return ((uint32_t)f < CPU_FEAT_COUNT) ? where[f].name : "?";
}

uint32_t cpufeat_describe(const struct cpu_info *ci, char *buf, uint32_t len) {
    if (!len) return 0;
    uint32_t at = 0;
    for (uint32_t f = 0; f < CPU_FEAT_COUNT; f++) {
        if (!(ci->feat & (1ull << f))) continue;
        const char *n = where[f].name;
        uint32_t nl = 0;
        while (n[nl]) nl++;
        uint32_t need = (at ? 1u : 0u) + nl;
        if (at + need + 5u > len) {        /* always room left for " ..." and the NUL */
            const char *more = at ? " ..." : "...";
            for (uint32_t i = 0; more[i] && at + 1u < len; i++) buf[at++] = more[i];
            break;
        }
        if (at) buf[at++] = ' ';
        for (uint32_t i = 0; i < nl; i++) buf[at++] = n[i];
    }
    buf[at] = 0;
    return at;
}

#ifndef CXK_HOST_TEST
#include "logging.h"

static struct cpu_info info;

void cpuid_read(uint32_t leaf, uint32_t subleaf, uint32_t r[4]) {
    __asm__ volatile ("cpuid"
                      : "=a"(r[R_EAX]), "=b"(r[R_EBX]), "=c"(r[R_ECX]), "=d"(r[R_EDX])
                      : "a"(leaf), "c"(subleaf));
}

void cpufeat_init(void) {
    struct cpuid_raw raw = { 0 };
    uint32_t r[4];
    cpuid_read(0, 0, r);
    raw.max_basic = r[R_EAX];
    raw.vendor[0] = r[R_EBX];
    raw.vendor[1] = r[R_EDX];
    raw.vendor[2] = r[R_ECX];
    if (raw.max_basic >= 1) cpuid_read(1, 0, raw.l1);
    if (raw.max_basic >= 7) cpuid_read(7, 0, raw.l7);
    cpuid_read(0x80000000u, 0, r);
    raw.max_ext = r[R_EAX];
    if (ext_has(raw.max_ext, 0x80000001u)) cpuid_read(0x80000001u, 0, raw.e1);
    if (ext_has(raw.max_ext, 0x80000007u)) cpuid_read(0x80000007u, 0, raw.e7);
    cpufeat_decode(&raw, &info);

    static char line[256];
    klog("CPU", SEV_OK, info.vendor);
    klog_child_u32("family ", info.family, LOG_COLOR_VALUE, "");
    klog_child_u32("model ", info.model, LOG_COLOR_VALUE, "");
    cpufeat_describe(&info, line, sizeof line);
    klog_child(line);
}

const struct cpu_info *cpu_info(void) { return &info; }

int cpu_has(enum cpu_feat f) {
    return (uint32_t)f < CPU_FEAT_COUNT && (info.feat >> f) & 1u;
}
#endif
