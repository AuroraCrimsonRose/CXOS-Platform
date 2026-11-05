// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_hwfmt.c */
/* Aurora Tejeda / CATX Systems */
/* The hardware report's form (diag/hwfmt.h): a report checks, a damaged one
   does not, a cut one says so, and firmware tables come out line for line,
   their checksums judged and their lengths not believed. */
#include "khost.h"
#include "diag/hwfmt.h"
#include "diag/crashrec.h"
#include <string.h>
#include <stdio.h>

static char buf[4096];

/* Does the report hold exactly this line? */
static int has_line(const char *t, uint32_t len, const char *line) {
    char want[256];
    snprintf(want, sizeof want, "%s\n", line);
    size_t n = strlen(want);
    for (uint32_t at = 0; at < len; ) {
        const char *e = memchr(t + at, '\n', len - at);
        if (!e) return 0;
        if ((size_t)(e - (t + at)) + 1 == n && memcmp(t + at, want, n) == 0) return 1;
        at = (uint32_t)(e - t) + 1;
    }
    return 0;
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* An SDT header for `sig`, `len` long, with its checksum made good. */
static void make_sdt(uint8_t *t, const char *sig, uint32_t len) {
    memset(t, 0, len);
    memcpy(t, sig, 4);
    put_le32(t + 4, len);
    t[8] = 3;
    memcpy(t + 10, "CXOEM ", 6);
    memcpy(t + 16, "TABLE   ", 8);
    put_le32(t + 24, 0x12345678u);
    t[9] = (uint8_t)(0u - hw_sum(t, len));
}

KH_TEST(hwfmt_round_trip) {
    struct hw_out o;
    hw_open(&o, buf, sizeof buf);
    hw_line(&o, "cpu"); hw_s(&o, "AuthenticAMD"); hw_x(&o, 0x15, 2); hw_done(&o);
    hw_line(&o, "clock"); hw_s(&o, "tsc-hz"); hw_u(&o, 4013000000ull); hw_done(&o);
    uint32_t len = hw_close(&o);
    KH_CHECK_EQ(hw_check(buf, len), HWR_OK);
    KH_CHECK(has_line(buf, len, "@hw\tbegin\t1"));
    KH_CHECK(has_line(buf, len, "@hw\tcpu\tAuthenticAMD\t15"));
    KH_CHECK(has_line(buf, len, "@hw\tclock\ttsc-hz\t4013000000"));
    char end[64];
    snprintf(end, sizeof end, "@hw\tend\t3\t%08x", xcrl_crc32(0, buf, len - 19));
    KH_CHECK(has_line(buf, len, end));
    KH_CHECK_EQ(o.over, 0);
}

KH_TEST(hwfmt_check_refuses_damage) {
    struct hw_out o;
    hw_open(&o, buf, sizeof buf);
    hw_line(&o, "pci"); hw_s(&o, "00:01.0"); hw_x(&o, 0x8086, 4); hw_done(&o);
    uint32_t len = hw_close(&o);
    static char c[4096];

    memcpy(c, buf, len); c[1] = 'H';                       /* begin line wrong */
    KH_CHECK_EQ(hw_check(c, len), HWR_NO_BEGIN);
    memcpy(c, buf, len); c[17] = 'X';                      /* a body byte: CRC */
    KH_CHECK_EQ(hw_check(c, len), HWR_BAD_CRC);
    memcpy(c, buf, len); c[20] = '\r';                     /* a CR in a line */
    KH_CHECK_EQ(hw_check(c, len), HWR_BAD_LINE);
    KH_CHECK_EQ(hw_check(buf, len - 1), HWR_BAD_LINE);     /* no final LF */
    KH_CHECK_EQ(hw_check(buf, 12), HWR_NO_END);            /* the begin line alone */

    memcpy(c, buf, len); memcpy(c + len, "junk\n", 5);     /* a line after the end */
    KH_CHECK_EQ(hw_check(c, len + 5), HWR_BAD_LINE);
    memcpy(c, buf, len); memcpy(c + len, "@hw\tx\n", 6);   /* an @hw line after it */
    KH_CHECK_EQ(hw_check(c, len + 6), HWR_NO_END);

    /* The count: a line taken out and the CRC made good again. */
    uint32_t pci_at = 12, pci_len = (uint32_t)((char *)memchr(buf + 12, '\n', len) - (buf + 12)) + 1;
    uint32_t body = len - pci_len;
    memcpy(c, buf, pci_at);
    memcpy(c + pci_at, buf + pci_at + pci_len, len - pci_at - pci_len);
    uint32_t end_at = body - 19;
    snprintf(c + end_at, 20, "@hw\tend\t3\t%08x\n", xcrl_crc32(0, c, end_at));
    KH_CHECK_EQ(hw_check(c, body), HWR_BAD_COUNT);
}

KH_TEST(hwfmt_cut_report_says_so) {
    static char small[160];
    struct hw_out o;
    hw_open(&o, small, sizeof small);
    for (int i = 0; i < 40; i++) { hw_line(&o, "mem"); hw_x(&o, (uint64_t)i, 16); hw_done(&o); }
    uint32_t len = hw_close(&o);
    KH_CHECK(len <= sizeof small);
    KH_CHECK_EQ(o.over, 1);
    KH_CHECK_EQ(hw_check(small, len), HWR_OK);
    KH_CHECK(has_line(small, len, "@hw\ttruncated"));
    KH_CHECK(has_line(small, len, "@hw\tmem\t0000000000000000"));
    KH_CHECK(!has_line(small, len, "@hw\tmem\t0000000000000027"));
}

KH_TEST(hwfmt_fields) {
    struct hw_out o;
    hw_open(&o, buf, sizeof buf);
    hw_line(&o, "t"); hw_sn(&o, "Pi\x01" "e\t  ", 7); hw_done(&o);   /* cleaned, trimmed */
    hw_line(&o, "t"); hw_sn(&o, "    ", 4); hw_done(&o);            /* nothing left: "-" */
    hw_line(&o, "t"); hw_sn(&o, "ab\0cd", 5); hw_done(&o);          /* stops at NUL */
    hw_line(&o, "t"); hw_x(&o, 0x1234, 2); hw_x(&o, 0, 16); hw_u(&o, 0); hw_done(&o);
    uint32_t len = hw_close(&o);
    KH_CHECK(has_line(buf, len, "@hw\tt\tPi.e."));
    KH_CHECK(has_line(buf, len, "@hw\tt\t-"));
    KH_CHECK(has_line(buf, len, "@hw\tt\tab"));
    KH_CHECK(has_line(buf, len, "@hw\tt\t34\t0000000000000000\t0"));
}

KH_TEST(hwfmt_tables_and_checksums) {
    static uint8_t t[128];
    struct hw_out o;
    hw_open(&o, buf, sizeof buf);
    make_sdt(t, "APIC", 100);
    hw_table(&o, "rsdt", 0x7FE1000, t, 1);
    t[50] ^= 1;
    hw_table(&o, "xsdt", 0x7FE1000, t, 1);
    hw_table(&o, "xsdt", 0x7FE1000, t, 0);
    put_le32(t + 4, 20);
    hw_table(&o, "rsdt", 0x7FE2000, t, 1);
    hw_table(&o, "fadt", 0xDEAD000, 0, 0);
    memset(t, 0, 64); memcpy(t, "FACS", 4); put_le32(t + 4, 64);
    hw_table(&o, "fadt", 0x7FE3000, t, 1);
    uint32_t len = hw_close(&o);
    KH_CHECK(has_line(buf, len, "@hw\tacpi\ttable\trsdt\tAPIC\t0000000007fe1000\t00000064\t03\tCXOEM\tTABLE\t12345678\tok"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\ttable\txsdt\tAPIC\t0000000007fe1000\t00000064\t03\tCXOEM\tTABLE\t12345678\tbad"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\ttable\txsdt\tAPIC\t0000000007fe1000\t00000064\t03\tCXOEM\tTABLE\t12345678\tunchecked"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\ttable\trsdt\tAPIC\t0000000007fe2000\t00000014\t03\tCXOEM\tTABLE\t12345678\tshort"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\ttable\tfadt\t-\t000000000dead000\t-\t-\t-\t-\t-\tunmapped"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\ttable\tfadt\tFACS\t0000000007fe3000\t00000040\t-\t-\t-\t-\tnosum"));
}

KH_TEST(hwfmt_rsdp) {
    uint8_t r[36];
    memset(r, 0, sizeof r);
    memcpy(r, "RSD PTR ", 8);
    memcpy(r + 9, "BOCHS ", 6);
    put_le32(r + 16, 0x7FE14D2u);
    r[8] = (uint8_t)(0u - hw_sum(r, 20));
    struct hw_out o;
    hw_open(&o, buf, sizeof buf);
    hw_rsdp(&o, 0xF5A10, r);                 /* ACPI 1.0: no XSDT */
    r[15] = 2;  r[8] = (uint8_t)(r[8] - 2);  /* revision 2, first sum kept good */
    put_le32(r + 24, 0x7FE1500u);
    put_le32(r + 20, 36);
    r[32] = (uint8_t)(0u - hw_sum(r, 36));
    hw_rsdp(&o, 0xF5A10, r);
    r[33] ^= 1;                              /* outside the first 20: only the second sum */
    hw_rsdp(&o, 0xF5A10, r);
    uint32_t len = hw_close(&o);
    KH_CHECK(has_line(buf, len, "@hw\tacpi\trsdp\t00000000000f5a10\t00\tBOCHS\t07fe14d2\t-\tok\t-"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\trsdp\t00000000000f5a10\t02\tBOCHS\t07fe14d2\t0000000007fe1500\tok\tok"));
    KH_CHECK(has_line(buf, len, "@hw\tacpi\trsdp\t00000000000f5a10\t02\tBOCHS\t07fe14d2\t0000000007fe1500\tok\tbad"));
}

KH_TEST(hwfmt_madt_entries_and_lies) {
    static uint8_t m[160];
    memset(m, 0, sizeof m);
    memcpy(m, "APIC", 4);
    put_le32(m + 36, 0xFEE00000u);
    put_le32(m + 40, 1);
    uint32_t off = 44;
    uint8_t cpu[8]   = { 0, 8, 1, 2, 1, 0, 0, 0 };
    uint8_t ioa[12]  = { 1, 12, 3, 0, 0, 0, 0xC0, 0xFE, 0, 0, 0, 0 };
    uint8_t iso[10]  = { 2, 10, 0, 0, 2, 0, 0, 0, 0x0D, 0 };
    uint8_t nmi[6]   = { 4, 6, 0xFF, 5, 0, 1 };
    uint8_t x2[16]   = { 9, 16, 0, 0, 0x10, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0 };
    uint8_t odd[4]   = { 0x7F, 4, 0, 0 };
    uint8_t shrt[6]  = { 1, 6, 0, 0, 0, 0 };          /* an I/O APIC entry too short */
    uint8_t zero[2]  = { 0, 0 };                      /* length 0: the walk stops */
    memcpy(m + off, cpu, 8);   off += 8;
    memcpy(m + off, ioa, 12);  off += 12;
    memcpy(m + off, iso, 10);  off += 10;
    memcpy(m + off, nmi, 6);   off += 6;
    memcpy(m + off, x2, 16);   off += 16;
    memcpy(m + off, odd, 4);   off += 4;
    memcpy(m + off, shrt, 6);  off += 6;
    uint32_t zero_at = off;
    memcpy(m + off, zero, 2);  off += 2;
    memcpy(m + off, cpu, 8);   off += 8;              /* never reached */

    struct hw_out o;
    hw_open(&o, buf, sizeof buf);
    hw_madt(&o, m, off);
    hw_madt(&o, m, 40);
    uint32_t len = hw_close(&o);
    KH_CHECK_EQ(hw_check(buf, len), HWR_OK);
    KH_CHECK(has_line(buf, len, "@hw\tmadt\thead\tfee00000\t00000001"));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tcpu\t01\t02\t00000001"));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tioapic\t03\tfec00000\t00000000"));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tiso\t00\t00\t00000002\t000d"));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tnmi\tff\t0005\t01"));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tx2cpu\t00000007\t00000010\t00000001"));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tother\t7f\t04"));
    char bad[64];
    snprintf(bad, sizeof bad, "@hw\tmadt\tbad\t%08x\t01\t00000006", zero_at - 6);
    KH_CHECK(has_line(buf, len, bad));
    snprintf(bad, sizeof bad, "@hw\tmadt\tbad\t%08x\t00\t00000000", zero_at);
    KH_CHECK(has_line(buf, len, bad));
    KH_CHECK(has_line(buf, len, "@hw\tmadt\tbad\t00000000\t-\t00000028"));
    /* one cpu line: the one past the zero-length entry was never read */
    uint32_t cpus = 0;
    for (const char *p = buf; (p = strstr(p, "\tmadt\tcpu\t")) != 0; p++) cpus++;
    KH_CHECK_EQ(cpus, 1u);
}
