// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_panicrep.c */
/* Aurora Tejeda / CATX Systems */
/* The panic report's text, the fit rule and the embedded symbol lookup
   (diag/panicrep.h, diag/ksym.h). That the code is READABLE is the DevKit's
   test, with a real QR reader: PanicScreenTests. */
#include "khost.h"
#include "diag/panicrep.h"
#include "diag/ksym.h"
#include <string.h>

/* memmem is not on every host. */
static const char *has(const char *h, size_t n, const char *needle, size_t m) {
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(h + i, needle, m) == 0) return h + i;
    return NULL;
}

/* The same pins as ktest_panic.c, so kernel and host encode one code. */
#define FIXED_TEXT_FNV   0x928280D0u
#define FIXED_QR_FNV     0x4B4B0B58u
#define FIXED_QR_MODULES 89u

KH_TEST(panic_qr_fixed_record_is_pinned) {
    static struct panic_report r;
    static char text[PANIC_QR_BYTES_MAX];
    static uint8_t qr[PANIC_QR_BUF_LEN], tmp[PANIC_QR_BUF_LEN];
    panic_test_report(&r);
    size_t len = panic_qr_text(&r, text, sizeof text);
    KH_CHECK_EQ(panic_fnv1a(PANIC_FNV_INIT, text, len), FIXED_TEXT_FNV);
    unsigned modules = panic_qr_encode(text, len, qr, tmp);
    KH_CHECK_EQ(modules, FIXED_QR_MODULES);
    KH_CHECK_EQ(panic_qr_fingerprint(qr, modules), FIXED_QR_FNV);
}

KH_TEST(panic_fit_rule_edges) {
    KH_CHECK_EQ(panic_qr_px(125 * 3, 117), 3u);
    KH_CHECK_EQ(panic_qr_px(125 * 3 - 1, 117), 0u);
    KH_CHECK_EQ(panic_qr_px(125 * 8, 117), 8u);
    KH_CHECK_EQ(panic_qr_px(125 * 9, 117), 8u);
    KH_CHECK_EQ(panic_qr_px(0, 117), 0u);
    KH_CHECK_EQ(panic_qr_px(1000, 0), 0u);
}

KH_TEST(panic_layout_keeps_text_and_code_apart) {
    struct panic_layout l;
    panic_layout_for(1024, 768, 8, 16, 117, &l);
    KH_CHECK(l.px >= 3);
    KH_CHECK(l.qr_x >= 16 + PANIC_TEXT_COLS * 8);
    KH_CHECK(l.qr_x + l.qr_side <= 1024);
    KH_CHECK(l.qr_y + l.qr_side <= 768);

    panic_layout_for(640, 480, 8, 16, 117, &l);
    KH_CHECK_EQ(l.px, 0u);
    KH_CHECK(l.text_cols > 0);

    panic_layout_for(1920, 1080, 8, 16, 21, &l);
    KH_CHECK_EQ(l.px, 8u);

    /* A screen too small for anything divides nothing and draws nothing. */
    panic_layout_for(4, 4, 8, 16, 117, &l);
    KH_CHECK_EQ(l.px, 0u);
    KH_CHECK_EQ(l.text_cols, 0u);
}

KH_TEST(panic_text_is_bounded_and_ascii) {
    static struct panic_report r;
    static char huge[8000], out[PANIC_QR_BYTES_MAX + 64];
    panic_test_report(&r);

    /* Control bytes, high bytes, and far more log than fits. */
    for (size_t i = 0; i < sizeof huge - 1; i++)
        huge[i] = i % 50 == 49 ? '\n' : i % 7 == 0 ? (char)0xE9 : (char)('a' + i % 26);
    huge[sizeof huge - 1] = 0;
    r.log = huge;
    r.why = "bad\x01reason\xFF";

    memset(out, 0x7F, sizeof out);
    size_t n = panic_qr_text(&r, out, sizeof out);
    KH_CHECK(n <= PANIC_QR_BYTES_MAX);
    KH_CHECK(n > 700);                    /* the room was used */
    for (size_t i = 0; i < n; i++)
        KH_CHECK(out[i] == '\n' || (out[i] >= 0x20 && out[i] <= 0x7E));
    KH_CHECK_EQ((unsigned char)out[n], 0x7Fu);   /* nothing past the end */
    KH_CHECK(has(out, n, "bad?reason?", 11) != NULL);

    /* And the longest text there can be still makes a code of version 25
       or below. */
    static uint8_t qr[PANIC_QR_BUF_LEN], tmp[PANIC_QR_BUF_LEN];
    static char full[PANIC_QR_BYTES_MAX];
    memset(full, 'x', sizeof full);
    KH_CHECK_EQ(panic_qr_encode(full, sizeof full, qr, tmp), 117u);
    KH_CHECK_EQ(panic_qr_encode(full, 0, qr, tmp), 0u);
    KH_CHECK_EQ(panic_qr_encode(full, PANIC_QR_BYTES_MAX + 1, qr, tmp), 0u);
}

KH_TEST(panic_text_keeps_the_newest_log_lines_whole) {
    static struct panic_report r;
    static char out[PANIC_QR_BYTES_MAX];
    panic_test_report(&r);
    r.log = "first\nsecond\nthird\n";

    /* Room for the header and only the last two lines. */
    size_t full = panic_qr_text(&r, out, sizeof out);
    size_t cut = panic_qr_text(&r, out, full - strlen("first\n"));
    KH_CHECK(has(out, cut, "first", 5) == NULL);
    KH_CHECK(has(out, cut, "second\nthird\n", 13) != NULL);

    /* No trailing newline is a line all the same. */
    r.log = "only";
    size_t n = panic_qr_text(&r, out, sizeof out);
    KH_CHECK(n >= 10 && memcmp(out + n - 10, "log:\nonly\n", 10) == 0);

    /* One enormous line is cut, not lost. */
    static char longline[400];
    memset(longline, 'z', sizeof longline - 1);
    r.log = longline;
    n = panic_qr_text(&r, out, sizeof out);
    KH_CHECK(has(out, n, "log:\n", 5) != NULL);
    KH_CHECK(n >= 81 && out[n - 1] == '\n' && out[n - 81] == 'z' && out[n - 82] == '\n');
}

KH_TEST(panic_stop_names_and_crash_ids) {
    KH_CHECK(strcmp(panic_stop_name(14), "KERNEL_PAGE_FAULT") == 0);
    KH_CHECK(strcmp(panic_stop_name(8), "KERNEL_DOUBLE_FAULT") == 0);
    KH_CHECK(strcmp(panic_stop_name(9), "KERNEL_CPU_EXCEPTION") == 0);
    KH_CHECK(strcmp(panic_stop_name(255), "KERNEL_CPU_EXCEPTION") == 0);

    KH_CHECK(strstr(panic_advice(1), "scan") != NULL);
    KH_CHECK(strstr(panic_advice(0), "note the stop code") != NULL);

    static struct panic_report a, b;
    panic_test_report(&a);
    panic_test_report(&b);
    KH_CHECK_EQ(panic_crash_id(&a), panic_crash_id(&b));
    b.frames[1].addr++;
    KH_CHECK(panic_crash_id(&a) != panic_crash_id(&b));
    panic_test_report(&b);
    b.why = "another reason";
    KH_CHECK(panic_crash_id(&a) != panic_crash_id(&b));
    /* A different thread hitting the same bug is the same crash. */
    panic_test_report(&b);
    b.tid = 99;
    KH_CHECK_EQ(panic_crash_id(&a), panic_crash_id(&b));
}

KH_TEST(ksym_finds_the_covering_symbol) {
    static const uint32_t offs[] = { 0x10, 0x20, 0x40 };
    static const uint32_t names[] = { 0, 2, 4 };
    static const char pool[] = "a\0b\0c";
    uint32_t off = 99;

    KH_CHECK(ksym_find(offs, names, pool, 3, 0xF, &off) == NULL);
    KH_CHECK(strcmp(ksym_find(offs, names, pool, 3, 0x10, &off), "a") == 0);
    KH_CHECK_EQ(off, 0u);
    KH_CHECK(strcmp(ksym_find(offs, names, pool, 3, 0x1F, &off), "a") == 0);
    KH_CHECK_EQ(off, 0xFu);
    KH_CHECK(strcmp(ksym_find(offs, names, pool, 3, 0x20, &off), "b") == 0);
    KH_CHECK_EQ(off, 0u);
    KH_CHECK(strcmp(ksym_find(offs, names, pool, 3, 0x41, &off), "c") == 0);
    KH_CHECK_EQ(off, 1u);
    KH_CHECK(ksym_find(offs, names, pool, 0, 0x41, &off) == NULL);
}
