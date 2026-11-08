// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_panic.c */
/* Aurora Tejeda / CATX Systems */
#include "ktest_panic.h"
#include "diag/panicrep.h"
#include "diag/panicscreen.h"
#include "diag/ksym.h"
#include "logging.h"
#include "string.h"
#include "panic.h"

/* Pinned by the host's decode of the same record: PanicScreenTests. */
#define FIXED_TEXT_FNV   0x928280D0u
#define FIXED_QR_FNV     0x4B4B0B58u
#define FIXED_QR_MODULES 89u

static int fail(const char *what) {
    klog("KTEST", SEV_FAIL, what);
    return 0;
}

int ktest_panic_fit(void) {
    const unsigned m = 117;                   /* version 25, the largest */
    const uint32_t need3 = (m + 8) * 3;
    if (panic_qr_px(need3, m) != 3) return fail("panic fit: exactly 3 px did not fit");
    if (panic_qr_px(need3 - 1, m) != 0) return fail("panic fit: under 3 px was drawn");
    if (panic_qr_px((m + 8) * 8, m) != 8 || panic_qr_px((m + 8) * 100, m) != 8)
        return fail("panic fit: not capped at 8 px");
    if (panic_qr_px(1000, 0) != 0) return fail("panic fit: a failed encode was drawn");

    struct panic_layout l;
    panic_layout_for(1024, 768, 8, 16, m, &l);
    if (l.px < 3) return fail("panic fit: 1024x768 shows no code");
    if (l.qr_x + l.qr_side > 1024 || l.qr_y + l.qr_side > 768)
        return fail("panic fit: the code runs off the screen");
    if (l.qr_x < 16 + PANIC_TEXT_COLS * 8) return fail("panic fit: the code covers the text");

    panic_layout_for(640, 480, 8, 16, m, &l);
    if (l.px != 0) return fail("panic fit: 640x480 drew a code");
    if (l.text_cols == 0) return fail("panic fit: 640x480 has no text");
    return 1;
}

int ktest_panic_qr(void) {
    static struct panic_report r;
    static char text[PANIC_QR_BYTES_MAX];
    static uint8_t qr[PANIC_QR_BUF_LEN], tmp[PANIC_QR_BUF_LEN];

    panic_test_report(&r);
    size_t len = panic_qr_text(&r, text, sizeof text);
    uint32_t tf = panic_fnv1a(PANIC_FNV_INIT, text, len);
    if (tf != FIXED_TEXT_FNV) {
        klog_u32("KTEST", SEV_FAIL, "panic qr: text fingerprint ", tf, 0x0F, 0);
        return 0;
    }
    unsigned modules = panic_qr_encode(text, len, qr, tmp);
    if (modules != FIXED_QR_MODULES) return fail("panic qr: wrong size of code");
    uint32_t qf = panic_qr_fingerprint(qr, modules);
    if (qf != FIXED_QR_FNV) {
        klog_u32("KTEST", SEV_FAIL, "panic qr: module fingerprint ", qf, 0x0F, 0);
        return 0;
    }
    return 1;
}

static int name_is(uintptr_t addr, const char *want, uint32_t want_off) {
    uint32_t off = 0xFFFFFFFFu;
    const char *n = ksym_lookup(addr, &off);
    return n && strcmp(n, want) == 0 && off == want_off;
}

int ktest_panic_ksym(void) {
    if (ksym_count < 100) return fail("ksym: the table is empty");
    for (uint32_t i = 1; i < ksym_count; i++)
        if (ksym_offs[i] <= ksym_offs[i - 1]) return fail("ksym: the table is not sorted");
    /* These hold only if the table's link is the running kernel's. */
    if (!name_is((uintptr_t)kpanic, "kpanic", 0)) return fail("ksym: kpanic is not at its address");
    if (!name_is((uintptr_t)ktest_panic_ksym, "ktest_panic_ksym", 0))
        return fail("ksym: this test is not at its address");
    if (!name_is((uintptr_t)ktest_panic_fit + 5, "ktest_panic_fit", 5))
        return fail("ksym: an address inside a function was misnamed");
    if (ksym_lookup(0, 0) != 0) return fail("ksym: address 0 was named");
    return 1;
}

/* ---- the walk: three functions that must stay three frames ---- */

static volatile unsigned walk_side;   /* a call followed by this is not a tail call */
static unsigned walked;
static uint64_t walk_pcs[PANIC_FRAMES_MAX];

__attribute__((noinline)) static void walk_c(void) {
    walked = panic_stack_walk((uint64_t)(uintptr_t)__builtin_frame_address(0), walk_pcs, PANIC_FRAMES_MAX);
    walk_side++;
}
__attribute__((noinline)) static void walk_b(void) { walk_c(); walk_side++; }
__attribute__((noinline)) static void walk_a(void) { walk_b(); walk_side++; }

static int walk_has(unsigned at, const char *want) {
    uint32_t off;
    if (at >= walked) return 0;
    const char *n = ksym_lookup((uintptr_t)walk_pcs[at], &off);
    return n && strcmp(n, want) == 0;
}

int ktest_panic_walk(void) {
    walk_a();
    if (walked < 3) return fail("walk: the chain was cut short");
    if (!walk_has(0, "walk_b") || !walk_has(1, "walk_a") || !walk_has(2, "ktest_panic_walk"))
        return fail("walk: the frames are not the callers, in order");

    /* A frame pointer that is unmapped, or misaligned, ends the walk. */
    if (panic_stack_walk(0, walk_pcs, PANIC_FRAMES_MAX) != 0) return fail("walk: followed a null frame");
    if (panic_stack_walk(0xFFFF900000000000ull, walk_pcs, PANIC_FRAMES_MAX) != 0)
        return fail("walk: followed an unmapped frame");
    /* A frame that would read as good, one byte off its alignment: only the
       alignment check can refuse it. */
    uint8_t fake[8 * 3] __attribute__((aligned(8)));
    uint64_t pc = (uint64_t)(uintptr_t)kpanic;
    for (unsigned i = 0; i < sizeof fake; i++) fake[i] = 0;
    for (unsigned i = 0; i < 8; i++) fake[1 + 8 + i] = (uint8_t)(pc >> (i * 8));
    if (panic_stack_walk((uint64_t)(uintptr_t)(fake + 1), walk_pcs, PANIC_FRAMES_MAX) != 0)
        return fail("walk: followed a misaligned frame");
    return 1;
}
