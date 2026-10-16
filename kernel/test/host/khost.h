// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/khost.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Kernel code, tested on the host.
 *
 * The boot self-tests (ktest*.c) prove the kernel works on the machine. These
 * prove the LOGIC of kernel code that has no hardware dependency - bitmaps,
 * hashes, handle tables, allocators - compiled for the host with clang and run
 * under AddressSanitizer and UndefinedBehaviorSanitizer, in seconds rather than
 * a boot (docs/safety/CX_SAFETY.md section 3.7).
 *
 * A test is a function declared with KH_TEST. It registers itself before main
 * runs, so adding a test is adding a function; there is no list to keep in
 * step. A failed KH_CHECK records the file and line and the test carries on,
 * so one run reports every broken check rather than the first.
 *
 * The sanitizers are part of the contract, not an option: an out-of-bounds
 * read or a signed overflow ends the run with a report even when every check
 * passed, which is the class of bug example-based checks miss.
 */
#ifndef KHOST_H
#define KHOST_H

#include <stdint.h>
#include <stddef.h>

typedef void (*kh_fn)(void);

struct kh_test {
    const char     *name;
    kh_fn           fn;
    struct kh_test *next;
};

void kh_register(struct kh_test *t);
void kh_fail(const char *file, int line, const char *what);

#define KH_TEST(name)                                                   \
    static void name(void);                                             \
    static struct kh_test kh_rec_##name = { #name, name, 0 };           \
    __attribute__((constructor)) static void kh_reg_##name(void) {      \
        kh_register(&kh_rec_##name);                                    \
    }                                                                   \
    static void name(void)

/* The check, the expression as text, and where. Carries on after a failure. */
#define KH_CHECK(cond)                                                  \
    do { if (!(cond)) kh_fail(__FILE__, __LINE__, #cond); } while (0)

/* Equality of two unsigned values, reporting both when they differ. */
void kh_fail_eq(const char *file, int line, const char *what,
                uint64_t got, uint64_t want);
#define KH_CHECK_EQ(got, want)                                          \
    do {                                                                \
        uint64_t kh_g_ = (uint64_t)(got), kh_w_ = (uint64_t)(want);     \
        if (kh_g_ != kh_w_)                                             \
            kh_fail_eq(__FILE__, __LINE__, #got " == " #want, kh_g_, kh_w_); \
    } while (0)

/* A deterministic generator for property tests: the same seed gives the same
   sequence on every host, so a failure found once is found again. */
static inline uint32_t kh_rand(uint64_t *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(*state >> 33);
}

#endif
