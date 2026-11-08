// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/panicrep.h */
/* Aurora Tejeda / CATX Systems */
/*
 * What a panic reports, as data (roadmap Phase 2 step 3a).
 *
 * Everything here is free of the machine, so the host tests run it under the
 * sanitizers: the stop codes, the crash number, the text the QR code carries,
 * the rule for whether the code fits, and the code itself. The panic path
 * fills a `struct panic_report`; kernel/diag/panicscreen.c draws it.
 *
 * Nothing here allocates or takes a lock, and the QR buffers are the caller's
 * static ones: a panic may be running on the double-fault stack.
 */
#ifndef DIAG_PANICREP_H
#define DIAG_PANICREP_H

#include <stddef.h>
#include <stdint.h>

#define PANIC_FRAMES_MAX    16u
#define PANIC_QR_FRAMES     5u     /* the short trace: on screen and in the code */

/* Version 25 is the largest code the spec allows, and at error correction M
   it holds 997 bytes in byte mode. The text is cut to that, so an encode can
   only fail if the encoder is broken. */
#define PANIC_QR_VERSION_MAX 25
#define PANIC_QR_BYTES_MAX   997u
#define PANIC_QR_BUF_LEN     1713u /* qrcodegen_BUFFER_LEN_FOR_VERSION(25), checked in panicrep.c */

#define PANIC_QR_QUIET       4u    /* modules of quiet zone on each side */
#define PANIC_QR_PX_MIN      3u    /* smaller is not drawn: a phone cannot read it */
#define PANIC_QR_PX_MAX      8u

#define PANIC_TEXT_COLS      64u   /* the text column, in characters */

/* The stop code a kpanic has; a CPU exception's is its vector. */
#define PANIC_STOP_CHECK     0x100u

struct panic_frame {
    uint64_t    addr;
    const char *name;     /* 0 when no symbol covers it */
    uint32_t    off;      /* from the symbol's start */
};

struct panic_report {
    const char *stop_name;   /* KERNEL_PAGE_FAULT */
    uint32_t    stop_code;
    const char *why;         /* the reason, in the kernel's words */
    uint32_t    crash_id;    /* panic_crash_id */

    const char *version;
    uint32_t    abi;

    uint32_t    cpu;
    int32_t     tid, pid;
    const char *thread;      /* 0 when unknown */

    int         has_regs;    /* a CPU exception's: the rest are zero for a kpanic */
    uint64_t    rip, rsp, rbp, cr2, error;
    uint64_t    rax, rbx, rcx, rdx, rsi, rdi;

    unsigned    nframes;
    struct panic_frame frames[PANIC_FRAMES_MAX];

    const char *log;         /* the system log's tail, newline-separated; 0 for none */
};

/* The stop code's name for CPU exception `vector`, always KERNEL_something. */
const char *panic_stop_name(uint32_t vector);

/* What the stop code means, in a sentence anyone can read. */
const char *panic_plain(uint32_t stop_code);

/* What to do. With the code on screen it says to scan it; without, to note
   the stop code and crash number. */
const char *panic_advice(int show_qr);

/* The same crash gives the same number, so two reports can be told apart or
   matched: FNV-1a of the stop code, the reason and the code addresses. */
uint32_t panic_crash_id(const struct panic_report *r);

/* The QR code's text: plain ASCII, the essentials first and the log's newest
   lines in whatever room is left. Never more than min(cap, PANIC_QR_BYTES_MAX)
   bytes. Returns the length; not terminated. */
size_t panic_qr_text(const struct panic_report *r, char *out, size_t cap);

/* Encodes `len` bytes in byte mode at error correction M, version 25 or
   below. `qr` and `tmp` are PANIC_QR_BUF_LEN each. Returns the modules per
   side, or 0 on failure. */
unsigned panic_qr_encode(const char *text, size_t len, uint8_t *qr, uint8_t *tmp);

/* The module at (x, y): 1 is dark. */
int panic_qr_module(const uint8_t *qr, unsigned x, unsigned y);

/* The fit rule: pixels per module for a code of `modules` in a square of
   `room` pixels, quiet zone included. 0 means the code is not drawn. */
unsigned panic_qr_px(uint32_t room, unsigned modules);

struct panic_layout {
    unsigned text_cols;
    unsigned px;          /* 0: no code */
    uint32_t qr_x, qr_y;  /* the quiet zone's top-left */
    uint32_t qr_side;     /* the quiet zone's side, pixels */
};

/* Where things go on a w x h screen with fw x fh characters. `modules` 0 is
   an encode that failed, which is laid out as no code. */
void panic_layout_for(uint32_t w, uint32_t h, uint32_t fw, uint32_t fh,
                      unsigned modules, struct panic_layout *out);

/* The record both test suites pin: the kernel's self-test against a
   fingerprint, the host's by decoding it with a real QR reader. */
void panic_test_report(struct panic_report *r);

/* FNV-1a over bytes, and over a code's modules in reading order. */
uint32_t panic_fnv1a(uint32_t h, const void *data, size_t n);
uint32_t panic_qr_fingerprint(const uint8_t *qr, unsigned modules);

#define PANIC_FNV_INIT 0x811C9DC5u

#endif
