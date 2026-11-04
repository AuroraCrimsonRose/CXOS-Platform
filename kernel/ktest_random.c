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
#include "diag/crashlog.h"
#include "diag/crashrec.h"
#include "diag/testhook.h"
#include "disk.h"
#include "sched.h"              /* thread_current_id */

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

/* ---- the seed file ---- */

static uint8_t  sf_rec[2 * XCRL_SECTOR];    /* header, then the seed's sector */
static uint8_t  sf_disk;
static uint64_t sf_lba;

/* The record for `s` in sf_rec, written raw: what a last boot left. */
static int sf_put(const uint8_t *s, int damaged) {
    for (uint32_t i = 0; i < sizeof sf_rec; i++) sf_rec[i] = 0;
    for (uint32_t i = 0; i < XCRL_SEED_LEN; i++) sf_rec[XCRL_SECTOR + i] = s[i];
    xcrl_seal(sf_rec, XCRL_KIND_SEED, (const char *)sf_rec + XCRL_SECTOR, XCRL_SEED_LEN);
    if (damaged) sf_rec[XCRL_SECTOR + 5] ^= 0x10;
    return disk_write(sf_disk, sf_lba + XCRL_SEED_LBA, 2, sf_rec) == DISK_OK
        && disk_flush(sf_disk) == DISK_OK;
}

/* At RP_SEED_WRITTEN: the old record back, as a disk that dropped the
   rewrite would still hold it. */
static void sf_drop(void *ctx) {
    (void)ctx;
    (void)disk_write(sf_disk, sf_lba + XCRL_SEED_LBA, 2, sf_rec);
}

static uint32_t sf_steps(void) {
    static uint8_t s[XCRL_SEED_LEN], got[XCRL_SEED_LEN], want[XCRL_SEED_LEN + 32], out[32];
    static struct fortuna g;
    struct random_stats a, b;
    if (crashlog_area(&sf_disk, &sf_lba, 0) != 0) return 1;
    for (uint32_t i = 0; i < XCRL_SEED_LEN; i++) s[i] = (uint8_t)(i * 37u + 11u);

    /* ---- unseeded, the rewrite read back: credited in full; the file
       ends holding the generator's first bytes, and the generator goes on
       from there ---- */
    if (!sf_put(s, 0)) return 2;
    random_test_hold(0);
    random_stats(&a);
    uint32_t bits = random_seed_file_load();
    int seeded = random_seeded();
    int rc = random_bytes(out, 32);
    random_stats(&b);
    random_test_release();
    if (bits != FORTUNA_SEED_BITS || !seeded || rc != 0) return 3;
    if (b.file_bits - a.file_bits != FORTUNA_SEED_BITS) return 4;
    fortuna_init(&g);
    fortuna_add(&g, s, XCRL_SEED_LEN, FORTUNA_SEED_BITS);
    if (fortuna_generate(&g, want, XCRL_SEED_LEN, 0) || fortuna_generate(&g, want + XCRL_SEED_LEN, 32, 0)) return 5;
    if (crashlog_seed_read(got) != CRASHLOG_SEED_OK) return 6;
    for (uint32_t i = 0; i < XCRL_SEED_LEN; i++) if (got[i] != want[i]) return 7;
    for (uint32_t i = 0; i < 32; i++) if (out[i] != want[XCRL_SEED_LEN + i]) return 8;

    /* ---- unseeded, the rewrite dropped by the disk: not credited ---- */
    if (!sf_put(s, 0)) return 9;
    if (race_arm(RP_SEED_WRITTEN, thread_current_id(), 1, RACE_CALL, sf_drop, 0) != 0) return 10;
    random_test_hold(0);
    random_stats(&a);
    bits = random_seed_file_load();
    seeded = random_seeded();
    rc = random_bytes(out, 32);
    random_stats(&b);
    random_test_release();
    race_release(RP_SEED_WRITTEN);
    if (race_arrivals(RP_SEED_WRITTEN) != 1) return 10;
    if (bits || seeded || rc != E_SHOULD_WAIT || b.file_bits != a.file_bits) return 11;
    if (crashlog_seed_read(got) != CRASHLOG_SEED_OK) return 12;
    for (uint32_t i = 0; i < XCRL_SEED_LEN; i++) if (got[i] != s[i]) return 12;

    /* ---- a damaged record: nothing credited; unseeded, nothing written ---- */
    if (!sf_put(s, 1)) return 13;
    random_test_hold(0);
    bits = random_seed_file_load();
    seeded = random_seeded();
    random_test_release();
    if (bits || seeded) return 14;
    if (crashlog_seed_read(got) != CRASHLOG_SEED_BAD) return 15;

    /* ---- seeded: the file takes the generator's bytes before the seed
       counts ---- */
    if (!sf_put(s, 0)) return 16;
    random_test_hold(SEED);
    bits = random_seed_file_load();
    random_test_release();
    uint64_t seed = SEED;
    fortuna_init(&g);
    fortuna_add(&g, &seed, sizeof seed, FORTUNA_SEED_BITS);
    if (fortuna_generate(&g, want, XCRL_SEED_LEN, 0)) return 17;
    if (bits != FORTUNA_SEED_BITS) return 18;
    if (crashlog_seed_read(got) != CRASHLOG_SEED_OK) return 19;
    for (uint32_t i = 0; i < XCRL_SEED_LEN; i++) if (got[i] != want[i]) return 20;
    return 0;
}

int ktest_random_seed_file(void) {
#if !CXK_ALLOW_DISK_WRITE
    klog("KTEST", SEV_INFO, "random: no seed file, this build writes to no disk");
    return 1;
#else
    uint32_t step = sf_steps();
    race_release(RP_SEED_WRITTEN);
    /* The next boot's seed from the real generator, not a test's. */
    int w = random_seed_file_save();
    if (!step && w != CRASHLOG_SEED_OK && w != E_SHOULD_WAIT) step = 21;
    if (step) return rn_fail("random: seed file failed at step ", step);
    return 1;
#endif
}

#else
int ktest_random_syscall(void) { return 1; }
int ktest_random_seed_file(void) { return 1; }
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
