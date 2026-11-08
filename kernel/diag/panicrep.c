// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/panicrep.c */
/* Aurora Tejeda / CATX Systems */
/* See panicrep.h. No kernel headers: the host tests compile this as it is. */
#include "diag/panicrep.h"
#include "qrcodegen.h"

#include <string.h>

_Static_assert(PANIC_QR_BUF_LEN == qrcodegen_BUFFER_LEN_FOR_VERSION(PANIC_QR_VERSION_MAX),
               "PANIC_QR_BUF_LEN is not the buffer version 25 needs");
_Static_assert(PANIC_QR_BYTES_MAX < PANIC_QR_BUF_LEN, "the text must fit the encoder's buffer");

/* ---- stop codes ---- */

const char *panic_stop_name(uint32_t vector) {
    switch (vector) {
    case 0:  return "KERNEL_DIVIDE_ERROR";
    case 1:  return "KERNEL_DEBUG_TRAP";
    case 2:  return "KERNEL_NMI";
    case 3:  return "KERNEL_BREAKPOINT";
    case 4:  return "KERNEL_OVERFLOW";
    case 5:  return "KERNEL_BOUND_RANGE";
    case 6:  return "KERNEL_INVALID_OPCODE";
    case 7:  return "KERNEL_DEVICE_NOT_AVAILABLE";
    case 8:  return "KERNEL_DOUBLE_FAULT";
    case 10: return "KERNEL_INVALID_TSS";
    case 11: return "KERNEL_SEGMENT_NOT_PRESENT";
    case 12: return "KERNEL_STACK_FAULT";
    case 13: return "KERNEL_GENERAL_PROTECTION";
    case 14: return "KERNEL_PAGE_FAULT";
    case 16: return "KERNEL_FPU_ERROR";
    case 17: return "KERNEL_ALIGNMENT_CHECK";
    case 18: return "KERNEL_MACHINE_CHECK";
    case 19: return "KERNEL_SIMD_FAULT";
    case 20: return "KERNEL_VIRTUALIZATION_FAULT";
    case 21: return "KERNEL_CONTROL_PROTECTION";
    default: return "KERNEL_CPU_EXCEPTION";
    }
}

const char *panic_plain(uint32_t stop_code) {
    switch (stop_code) {
    case PANIC_STOP_CHECK: return "The kernel found that something inside it had gone wrong.";
    case 8:                return "The processor hit a fault while handling another fault.";
    case 12:
    case 14:               return "The kernel used memory that it should not have.";
    case 18:               return "The processor reported a hardware error.";
    default:               return "The processor stopped the kernel for doing something invalid.";
    }
}

const char *panic_advice(int show_qr) {
    return show_qr
        ? "Restart the computer. If this keeps happening, scan the code and send what it says."
        : "Restart the computer. If this keeps happening, note the stop code and crash number.";
}

/* ---- the crash number ---- */

uint32_t panic_fnv1a(uint32_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

uint32_t panic_crash_id(const struct panic_report *r) {
    uint32_t h = panic_fnv1a(PANIC_FNV_INIT, &r->stop_code, sizeof r->stop_code);
    if (r->why) h = panic_fnv1a(h, r->why, strlen(r->why));
    h = panic_fnv1a(h, &r->rip, sizeof r->rip);
    for (unsigned i = 0; i < r->nframes && i < PANIC_FRAMES_MAX; i++)
        h = panic_fnv1a(h, &r->frames[i].addr, sizeof r->frames[i].addr);
    return h;
}

/* ---- the QR code's text ---- */

struct w { char *p; size_t n, cap; };

static void w_c(struct w *w, char c) { if (w->n < w->cap) w->p[w->n++] = c; }

/* Plain ASCII on one line, whatever a corrupted string holds. */
static void w_s(struct w *w, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        w_c(w, c < 0x20 || c > 0x7E ? '?' : (char)c);
    }
}

static void w_lit(struct w *w, const char *s) { while (*s) w_c(w, *s++); }

static void w_sn(struct w *w, const char *s, size_t max) {
    if (!s) return;
    for (size_t i = 0; i < max && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        w_c(w, c < 0x20 || c > 0x7E ? '?' : (char)c);
    }
}

static void w_hex(struct w *w, uint64_t v, int digits) {
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        w_c(w, "0123456789abcdef"[(v >> i) & 0xF]);
}

/* The stop code and crash number, spelled as the screen and the serial report
   spell them: someone will compare the two. */
static void w_code(struct w *w, uint64_t v, int digits) {
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        w_c(w, "0123456789ABCDEF"[(v >> i) & 0xF]);
}

static void w_dec(struct w *w, uint64_t v) {
    char d[20];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) w_c(w, d[--n]);
}

static void w_reg(struct w *w, const char *name, uint64_t v) {
    w_s(w, name);
    w_c(w, ' ');
    w_hex(w, v, 16);
}

#define LOG_LINE_MAX 80u

/* The log's newest whole lines that fit, in order. Lines are cut to
   LOG_LINE_MAX first, so one long line cannot use all the room. */
static void w_log(struct w *w, const char *log) {
    static const char head[] = "log:\n";
    if (!log || !*log || w->n + sizeof head - 1 >= w->cap) return;
    size_t room = w->cap - w->n - (sizeof head - 1);

    size_t end = strlen(log);
    size_t first = end, used = 0;
    while (first > 0) {
        size_t e = first;
        if (log[e - 1] == '\n') e--;        /* this line's end */
        size_t s = e;
        while (s > 0 && log[s - 1] != '\n') s--;
        size_t len = e - s;
        if (len > LOG_LINE_MAX) len = LOG_LINE_MAX;
        if (used + len + 1 > room) break;
        used += len + 1;
        first = s;
    }
    if (first == end) return;

    w_lit(w, "log:\n");
    while (first < end) {
        size_t s = first;
        while (first < end && log[first] != '\n') first++;
        size_t len = first - s;
        w_sn(w, log + s, len > LOG_LINE_MAX ? LOG_LINE_MAX : len);
        w_c(w, '\n');
        if (first < end) first++;           /* past the newline */
    }
}

size_t panic_qr_text(const struct panic_report *r, char *out, size_t cap) {
    struct w w = { out, 0, cap > PANIC_QR_BYTES_MAX ? PANIC_QR_BYTES_MAX : cap };

    w_lit(&w, "CXOS crash report\nstop: ");
    w_s(&w, r->stop_name);
    w_lit(&w, " (CX-");
    w_code(&w, r->stop_code, 4);
    w_lit(&w, ")\ncrash: ");
    w_code(&w, r->crash_id, 8);
    w_lit(&w, "\nversion: ");
    w_s(&w, r->version);
    w_lit(&w, " abi ");
    w_dec(&w, r->abi);
    w_lit(&w, "\nreason: ");
    w_sn(&w, r->why, 120);
    w_lit(&w, "\ncpu ");
    w_dec(&w, r->cpu);
    w_lit(&w, " thread ");
    w_dec(&w, (uint64_t)(uint32_t)r->tid);
    if (r->thread) { w_c(&w, ' '); w_sn(&w, r->thread, 24); }
    w_lit(&w, " pid ");
    w_dec(&w, (uint64_t)(uint32_t)r->pid);
    w_c(&w, '\n');
    if (r->has_regs) {
        w_reg(&w, "rip", r->rip); w_reg(&w, " rsp", r->rsp); w_c(&w, '\n');
        w_reg(&w, "rbp", r->rbp); w_reg(&w, " cr2", r->cr2); w_c(&w, '\n');
        w_reg(&w, "err", r->error); w_reg(&w, " rax", r->rax); w_c(&w, '\n');
        w_reg(&w, "rbx", r->rbx); w_reg(&w, " rcx", r->rcx); w_c(&w, '\n');
        w_reg(&w, "rdx", r->rdx); w_reg(&w, " rsi", r->rsi); w_c(&w, '\n');
        w_reg(&w, "rdi", r->rdi); w_c(&w, '\n');
    }
    w_lit(&w, "trace:\n");
    for (unsigned i = 0; i < r->nframes && i < PANIC_QR_FRAMES; i++) {
        w_hex(&w, r->frames[i].addr, 16);
        if (r->frames[i].name) {
            w_c(&w, ' ');
            w_sn(&w, r->frames[i].name, 32);
            w_lit(&w, "+0x");
            w_hex(&w, r->frames[i].off, 4);
        }
        w_c(&w, '\n');
    }
    w_log(&w, r->log);
    return w.n;
}

/* ---- the code ---- */

unsigned panic_qr_encode(const char *text, size_t len, uint8_t *qr, uint8_t *tmp) {
    if (len == 0 || len > PANIC_QR_BYTES_MAX) return 0;
    memcpy(tmp, text, len);
    /* Binary, not encodeText: encodeText would pick numeric or alphanumeric
       for a text that allowed it, and the spec is byte mode. boostEcl false:
       the error correction is the spec's M, not whatever room allows. */
    if (!qrcodegen_encodeBinary(tmp, len, qr, qrcodegen_Ecc_MEDIUM,
                                qrcodegen_VERSION_MIN, PANIC_QR_VERSION_MAX,
                                qrcodegen_Mask_AUTO, false))
        return 0;
    return (unsigned)qrcodegen_getSize(qr);
}

int panic_qr_module(const uint8_t *qr, unsigned x, unsigned y) {
    return qrcodegen_getModule(qr, (int)x, (int)y) ? 1 : 0;
}

unsigned panic_qr_px(uint32_t room, unsigned modules) {
    if (modules == 0) return 0;
    uint32_t px = room / (modules + 2 * PANIC_QR_QUIET);
    if (px > PANIC_QR_PX_MAX) px = PANIC_QR_PX_MAX;
    return px < PANIC_QR_PX_MIN ? 0 : px;
}

void panic_layout_for(uint32_t w, uint32_t h, uint32_t fw, uint32_t fh,
                      unsigned modules, struct panic_layout *out) {
    (void)fh;
    uint32_t margin = fw * 2;
    uint32_t avail = w > 2 * margin ? w - 2 * margin : 0;
    uint32_t cols = fw ? avail / fw : 0;
    uint32_t text_w = fw * PANIC_TEXT_COLS;

    /* The right column is what the text column leaves, less a gutter. */
    uint32_t right = avail > text_w + margin ? avail - text_w - margin : 0;
    uint32_t tall = h > 2 * margin ? h - 2 * margin : 0;
    uint32_t room = right < tall ? right : tall;

    out->text_cols = cols < PANIC_TEXT_COLS ? cols : PANIC_TEXT_COLS;
    out->px = panic_qr_px(room, modules);
    out->qr_side = out->px * (modules + 2 * PANIC_QR_QUIET);
    out->qr_x = out->px ? w - margin - out->qr_side : 0;
    out->qr_y = out->px ? margin : 0;
}

/* ---- the fixed record ---- */

uint32_t panic_qr_fingerprint(const uint8_t *qr, unsigned modules) {
    uint32_t h = panic_fnv1a(PANIC_FNV_INIT, &modules, sizeof modules);
    for (unsigned y = 0; y < modules; y++)
        for (unsigned x = 0; x < modules; x++) {
            uint8_t m = (uint8_t)panic_qr_module(qr, x, y);
            h = panic_fnv1a(h, &m, 1);
        }
    return h;
}

void panic_test_report(struct panic_report *r) {
    memset(r, 0, sizeof *r);
    r->stop_name = "KERNEL_PAGE_FAULT";
    r->stop_code = 14;
    r->why       = "page fault";
    r->version   = "9.9.9";
    r->abi       = 34;
    r->cpu       = 2;
    r->tid       = 7;
    r->pid       = 3;
    r->thread    = "shell";
    r->has_regs  = 1;
    r->rip   = 0xFFFFFFFF80102030ull;
    r->rsp   = 0xFFFFFFFF80A00F00ull;
    r->rbp   = 0xFFFFFFFF80A00F40ull;
    r->cr2   = 0xDEAD0000ull;
    r->error = 2;
    r->rax   = 1; r->rbx = 2; r->rcx = 3; r->rdx = 4; r->rsi = 5; r->rdi = 6;
    r->nframes = 3;
    r->frames[0] = (struct panic_frame){ 0xFFFFFFFF80102030ull, "cxfs_read_file", 0x30 };
    r->frames[1] = (struct panic_frame){ 0xFFFFFFFF80103010ull, "syscall_dispatch", 0x110 };
    r->frames[2] = (struct panic_frame){ 0xFFFFFFFF80104000ull, 0, 0 };
    r->log = "KERNEL: online\nFS/CXFS: mounted the system volume\nSCHED: thread 7 started\n";
    r->crash_id = panic_crash_id(r);
}
