// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_random.c */
/* Aurora Tejeda / CATX Systems */
/* The kernel's random numbers, as a boot sees them (ktest_random.h). */

#include <stdint.h>
#include "config.h"
#include "ktest_random.h"
#include "random.h"
#include "usermode.h"
#include "systable.h"
#include "timer.h"
#include "logging.h"
#include "sync/spinlock.h"
#include "arch/cpu.h"
#include "lib/crypto/chacha20.h"
#include "lib/crypto/fortuna.h"

/* The reason first: the line `cxk test sabotage` reads as the step. */
static int rn_fail(const char *what, uint32_t step) {
    klog_u32("KTEST", SEV_FAIL, what, step, LOG_COLOR_VALUE, "");
    return 0;
}

int ktest_random_chacha(void) {
    static const uint8_t want[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
    };
    uint8_t key[32], out[64];
    const uint8_t nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    chacha20_block(key, 1, nonce, out);
    for (int i = 0; i < 64; i++)
        if (out[i] != want[i]) return rn_fail("chacha20: not RFC 8439's block at byte ", (uint32_t)i);
    return 1;
}

int ktest_random_interrupts(void) {
    struct random_stats a, b;
    /* Drained first: the ring fills within RING ticks of a request and then
       drops what comes, as it should. */
    (void)random_seeded();
    random_stats(&a);
    /* The timer runs on deadlines while the CPU idles: a sleep brings its
       own wakeup interrupt, not one a millisecond. */
    timer_sleep(20);
    random_stats(&b);
    if (b.taken - a.taken < 1)
        return rn_fail("random: interrupt samples in 20 ms: ", b.taken - a.taken);

    /* With interrupts off, so none lands between the request and the look. */
    uint32_t f = spin_irq_save();
    (void)random_seeded();                    /* drains, seeded or not */
    random_stats(&b);
    spin_irq_restore(f);
    if (b.pending) return rn_fail("random: samples left unhashed: ", b.pending);
    return 1;
}

#if CXK_TEST_HOOKS

#define SEED  0x5EEDF00DCAFE1234ull
#define DATA  USERMODE_DATA_VIRT
#define HOSTILE 0xFFFF8000DEAD0FF0ull

static void rn_set(uint64_t c[6], uint64_t nr, uint64_t a1, uint64_t a2) {
    c[0] = nr; c[1] = a1; c[2] = a2; c[3] = 0; c[4] = 0; c[5] = 0;
}

/* Two SYS_THREAD_IDs, which take nothing, then one read. */
static int rn_read(uint64_t buf, uint64_t len, uint8_t *data, uint32_t dlen) {
    struct um_call3 c;
    rn_set(c.c[0], SYS_THREAD_ID, 0, 0);
    rn_set(c.c[1], SYS_THREAD_ID, 0, 0);
    rn_set(c.c[2], SYS_RANDOM_READ, buf, len);
    return usermode_run_calls(&c, data, dlen, 0, 0);
}

static uint32_t rn_steps(uint8_t *d, int was) {
    /* ---- unseeded, so a check skipped asks the generator and is refused
       with E_SHOULD_WAIT, whatever the boot's own generator is ---- */
    random_test_hold(0);
    for (uint32_t i = 0; i < 64; i++) d[i] = 0xA5;
    int rc = rn_read(DATA, 32, d, 64);
    int rc0 = rn_read(DATA, 0, d, 64);
    int rcmax = rn_read(DATA, RANDOM_READ_MAX + 1, d, 64);
    random_test_release();
    if (rc != E_SHOULD_WAIT) return 1;
    for (uint32_t i = 0; i < 64; i++) if (d[i] != 0xA5) return 2;
    if (rc0 != 0) return 3;
    if (rcmax != E_OUT_OF_RANGE) return 4;
    /* The boot's generator back: seeded or not as before (one of this and
       step 11 sees a lost release, whichever the boot's was). */
    if (random_seeded() != was) return 10;

    /* ---- seeded from SEED alone: three reads, then the same from a
       generator seeded the same way here ---- */
    struct um_call3 c;
    rn_set(c.c[0], SYS_RANDOM_READ, DATA, 32);
    rn_set(c.c[1], SYS_RANDOM_READ, DATA + 32, 32);
    rn_set(c.c[2], SYS_RANDOM_READ, DATA + 64, RANDOM_READ_MAX);
    for (uint32_t i = 0; i < 64 + RANDOM_READ_MAX; i++) d[i] = 0xA5;
    random_test_hold(SEED);
    rc = usermode_run_calls(&c, d, 64 + RANDOM_READ_MAX, 0, 0);
    random_test_release();
    if (rc != (int)RANDOM_READ_MAX) return 5;

    static struct fortuna g;
    static uint8_t want[64 + RANDOM_READ_MAX];
    uint64_t seed = SEED;
    fortuna_init(&g);
    fortuna_add(&g, &seed, sizeof seed, FORTUNA_SEED_BITS);
    /* Close enough in time that no reseed comes between: none can, with
       nothing in the pools. */
    if (fortuna_generate(&g, want, 32, 0) || fortuna_generate(&g, want + 32, 32, 0)
        || fortuna_generate(&g, want + 64, RANDOM_READ_MAX, 0)) return 6;
    for (uint32_t i = 0; i < 64 + RANDOM_READ_MAX; i++) if (d[i] != want[i]) return 7;
    uint32_t same = 0;
    for (uint32_t i = 0; i < 32; i++) same += d[i] == d[32 + i];
    if (same == 32) return 8;

    /* A kernel address: refused at the copy, not by a fault in ring 0. */
    uint32_t faults = usermode_faults();
    random_test_hold(SEED);
    rc = rn_read(HOSTILE, 32, d, 64);
    random_test_release();
    if (rc != E_BAD_ADDRESS || usermode_faults() != faults) return 9;
    if (random_seeded() != was) return 11;
    return 0;
}

int ktest_random_syscall(void) {
    static uint8_t d[64 + RANDOM_READ_MAX];
    uint32_t step = rn_steps(d, random_seeded());
    if (step) return rn_fail("random_read failed at step ", step);
    return 1;
}

#else
int ktest_random_syscall(void) { return 1; }
#endif

int ktest_random_boot_seed(void) {
    struct random_stats s;
    random_stats(&s);
    int cpu = arch_cpu_has_random();
    klog_u32("KTEST", SEV_INFO, "random: boot credited, RDSEED/RDRAND ", s.cpu_bits, LOG_COLOR_VALUE, "");
    klog_child_u32("jitter ", s.jitter_bits, LOG_COLOR_VALUE, "");
    klog_child_u32("interrupts ", s.irq_bits, LOG_COLOR_VALUE, "");
    if (cpu && s.cpu_bits < FORTUNA_SEED_BITS) return rn_fail("random: CPU words credited only ", s.cpu_bits);
    if (cpu && !s.seeded) return rn_fail("random: CPU words, yet unseeded: ", s.cpu_bits);
    if (!cpu && s.cpu_bits) return rn_fail("random: credited a CPU source it lacks: ", s.cpu_bits);
    return 1;
}
