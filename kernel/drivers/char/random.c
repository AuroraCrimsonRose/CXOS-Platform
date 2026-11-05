// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/char/random.c */
/* Aurora Tejeda / CATX Systems */
/* The kernel's random numbers: sources, the lock, SYS_RANDOM_READ (random.h). */

#include <stdint.h>
#include "random.h"
#include "timer.h"
#include "logging.h"
#include "usermode.h"          /* user_copy_out */
#include "systable.h"          /* syscall_random_read */
#include "sync/spinlock.h"
#include "arch/cpu_ops.h"      /* arch_cycles32/64 */
#include "arch/cpu.h"         /* arch_cpu_random */
#include "boot/cxbi.h"         /* the loader's rng_seed */
#include "lib/crypto/fortuna.h"
#include "lib/crypto/sha256.h"
#include "diag/crashlog.h"     /* the seed file */
#include "diag/crashrec.h"     /* XCRL_SEED_LEN */
#include "disk.h"              /* DISK_ERR_NO_DEVICE */

#define RING          128      /* samples between drains; more are dropped */
#define JITTER_MAX    8192     /* boot loop rounds at most */
#define CPU_WORDS     4        /* 64-bit words asked of RDSEED / RDRAND */
#define CPU_TRIES     10       /* RDSEED may run dry for a moment */

struct sample { uint32_t tsc, vector; };

static struct sample ring[RING];
static volatile uint32_t ring_n;
static uint32_t taken, dropped;

static spinlock_t lock = SPINLOCK_INIT(LR_RANDOM);
static struct fortuna gen;
static struct jitter_est irq_est;
static uint32_t cpu_bits, jitter_bits, irq_bits, file_bits;
static int file_stale;         /* the seed file wants the generator's bytes, once seeded */

void random_irq_sample(uint32_t vector) {
    uint32_t n = ring_n;
    if (n == RING) { dropped++; return; }
    ring[n].tsc = arch_cycles32();
    ring[n].vector = vector;
    ring_n = n + 1;
    taken++;
}

#if CXK_TEST_HOOKS
static struct fortuna saved;
static int held;
#endif

/* Hash the stored samples in. With the lock held (interrupts off, so the
   ring stands still). */
static void drain(void) {
#if CXK_TEST_HOOKS
    if (held) return;
#endif
    uint32_t n = ring_n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = jitter_credit(&irq_est, ring[i].tsc);
        if (!gen.seeded) irq_bits += b;
        fortuna_add(&gen, &ring[i], sizeof ring[i], b);
    }
    ring_n = 0;
}

/* The CPU's words (RDSEED's, else RDRAND's); each plausible one credited
   in full. */
static uint32_t cpu_words(void) {
    uint64_t prev = 0;
    uint32_t bits = 0;
    if (!arch_cpu_has_random()) return 0;
    for (int k = 0; k < CPU_WORDS; k++) {
        uint64_t v = 0;
        int ok = 0;
        for (int t = 0; t < CPU_TRIES && !ok; t++) {
            ok = arch_cpu_random(&v);
            if (!ok) arch_cpu_relax();
        }
        if (!ok) continue;
        uint32_t b = rng_word_plausible(v, prev) ? 64 : 0;
        fortuna_add(&gen, &v, sizeof v, b);
        bits += b;
        prev = v;
    }
    return bits;
}

/* The cycle counter after each round of a loop of data-dependent reads and
   writes over a page: caches, the memory bus and whatever interrupts land
   in it make the rounds' lengths wander, on real hardware. With interrupts
   on, as kmain has them here. */
static uint8_t jpage[4096];

static uint32_t boot_jitter(void) {
    struct jitter_est e = { 0 };
    uint32_t bits = 0, x = 0;
    for (uint32_t i = 0; i < JITTER_MAX && !gen.seeded; i++) {
        uint32_t t0 = arch_cycles32();
        for (uint32_t j = 0; j < 64; j++) {
            x += jpage[(j * 67u + x) & 4095u];
            jpage[(j * 131u + i) & 4095u] ^= (uint8_t)x;
        }
        uint32_t t1 = arch_cycles32();
        uint32_t s[2] = { t1, t1 - t0 };
        uint32_t b = jitter_credit(&e, t1);
        fortuna_add(&gen, s, sizeof s, b);
        bits += b;
    }
    return bits;
}

void random_init(void) {
    /* Nothing else uses the generator yet, and the interrupts only fill the
       ring: no lock until it is published. */
    fortuna_init(&gen);
    const struct cxk_boot_info *bi = cxbi();
    uint64_t mix[3] = { bi ? bi->rng_seed : 0, arch_cycles64(), timer_now_ns() };
    fortuna_add(&gen, mix, sizeof mix, 0);      /* uncredited: nothing vouches for it */
    cpu_bits = cpu_words();
    jitter_bits = boot_jitter();

    uint32_t f = spin_lock(&lock);
    drain();
    int seeded = (int)gen.seeded;
    spin_unlock(&lock, f);

    if (seeded) {
        klog_u32("RANDOM", SEV_OK, "seeded at boot: RDSEED/RDRAND bits ", cpu_bits, LOG_COLOR_VALUE, "");
        klog_child_u32("jitter bits ", jitter_bits, LOG_COLOR_VALUE, "");
    } else {
        klog_u32("RANDOM", SEV_WARN, "not seeded at boot: credited bits ", gen.credit, LOG_COLOR_VALUE, " of 256");
        klog_child("random_read waits on interrupt timing (E_SHOULD_WAIT)");
    }
}

int random_bytes(void *out, uint32_t len) {
    uint32_t f = spin_lock(&lock);
    drain();
    int rc = fortuna_generate(&gen, out, len, timer_now_ns());
    spin_unlock(&lock, f);
    return rc == 0 ? 0 : E_SHOULD_WAIT;
}

int random_seeded(void) {
    uint32_t f = spin_lock(&lock);
    drain();
    int s = (int)gen.seeded;
    spin_unlock(&lock, f);
    return s;
}

void random_stats(struct random_stats *s) {
    uint32_t f = spin_lock(&lock);
    s->seeded = gen.seeded;
    s->cpu_bits = cpu_bits;
    s->jitter_bits = jitter_bits;
    s->irq_bits = irq_bits;
    s->file_bits = file_bits;
    s->taken = taken;
    s->dropped = dropped;
    s->pending = ring_n;
    s->reseeds = gen.reseeds;
    spin_unlock(&lock, f);
}

int syscall_random_read(uint64_t buf, uint64_t len) {
    if (len > RANDOM_READ_MAX) return E_OUT_OF_RANGE;
    if (len == 0) return 0;
    uint8_t k[RANDOM_READ_MAX];
    int rc = random_bytes(k, (uint32_t)len);
    /* An unwritable buffer costs the bytes made for it, nothing more: the
       copy is the check (usermode.h). */
    if (rc == 0) rc = user_copy_out(buf, k, (uint32_t)len) == 0 ? (int)len : E_BAD_ADDRESS;
    for (uint32_t i = 0; i < RANDOM_READ_MAX; i++) ((volatile uint8_t *)k)[i] = 0;
    return rc;
}

/* ---- the seed file ----
   Read once a boot, after the disks (crashlog.c keeps it in the crash
   partition). The seed is credited only after the file has been rewritten
   and the rewrite read back: a stick that drops writes would otherwise hand
   every boot the same seed, credited in full each time. */

static void wipe(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) ((volatile uint8_t *)p)[i] = 0;
}

/* What replaces a seed before the generator is seeded: SHA-256 of a label,
   a counter and the seed, so the file never holds the same seed twice and
   the new one does not give back the old. Rewritten from the generator once
   it is seeded (file_stale). */
static void next_from(uint8_t next[XCRL_SEED_LEN], const uint8_t old[XCRL_SEED_LEN]) {
    static const char label[] = "CXOS seed file, next";
    for (uint8_t k = 0; k < XCRL_SEED_LEN / SHA256_DIGEST_SIZE; k++) {
        struct sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, label, sizeof label);
        sha256_update(&c, &k, 1);
        sha256_update(&c, old, XCRL_SEED_LEN);
        sha256_final(&c, next + k * SHA256_DIGEST_SIZE);
        wipe((uint8_t *)&c, sizeof c);
    }
}

int random_seed_file_save(void) {
    static uint8_t next[XCRL_SEED_LEN];
    if (random_bytes(next, XCRL_SEED_LEN) != 0) { file_stale = 1; return E_SHOULD_WAIT; }
    int w = crashlog_seed_write(next);
    wipe(next, sizeof next);
    file_stale = w != CRASHLOG_SEED_OK;
    return w;
}

void random_seed_file_refresh(void) {
    if (file_stale && random_seeded()) (void)random_seed_file_save();
}

/* A self-test's loads run against a held generator and log nothing: the
   boot's own lines are the ones `cxk verify-seed` reads. */
static int seed_quiet(void) {
#if CXK_TEST_HOOKS
    return held;
#else
    return 0;
#endif
}

uint32_t random_seed_file_load(void) {
    static uint8_t old[XCRL_SEED_LEN], next[XCRL_SEED_LEN];
    int r = crashlog_seed_read(old);
    if (r == DISK_ERR_NO_DEVICE) {
        if (!seed_quiet()) klog("RANDOM", SEV_INFO, "no crash area: no seed file");
        return 0;
    }
    if (r != CRASHLOG_SEED_OK) {
        int w = random_seed_file_save();
        if (seed_quiet()) return 0;
        if (r == CRASHLOG_SEED_BAD) klog("RANDOM", SEV_WARN, "seed file damaged: not used");
        else if (r != CRASHLOG_SEED_NONE)
            klog_u32("RANDOM", SEV_WARN, "seed file unreadable, disk error ", (uint32_t)-r, LOG_COLOR_VALUE, "");
        if (w == CRASHLOG_SEED_OK) klog("RANDOM", SEV_OK, "seed file written for the next boot");
        else klog("RANDOM", SEV_WARN, "no seed file for the next boot yet: written once seeded");
        return 0;
    }

    /* The file is rewritten before the seed counts: from the generator if
       it is seeded already, else from the seed itself. */
    int fresh = random_bytes(next, XCRL_SEED_LEN) == 0;
    if (!fresh) next_from(next, old);
    int w = crashlog_seed_write(next);
    uint32_t bits = w == CRASHLOG_SEED_OK ? FORTUNA_SEED_BITS : 0;

    uint32_t f = spin_lock(&lock);
    drain();
    fortuna_add(&gen, old, XCRL_SEED_LEN, bits);
    file_bits += bits;
    spin_unlock(&lock, f);
    wipe(old, sizeof old);
    wipe(next, sizeof next);

    file_stale = !fresh || w != CRASHLOG_SEED_OK;
    if (bits) random_seed_file_refresh();
    if (seed_quiet()) return bits;
    if (bits) {
        klog_u32("RANDOM", SEV_OK, "seed file read back rewritten, credited bits ", bits, LOG_COLOR_VALUE, "");
    } else {
        klog_u32("RANDOM", SEV_WARN, "seed file not credited: its rewrite failed, ", (uint32_t)(w < 0 ? -w : w), LOG_COLOR_VALUE, "");
        klog_child("mixed in uncredited (a disk that drops writes replays it)");
    }
    return bits;
}

#if CXK_TEST_HOOKS
static uint32_t saved_file_bits;

void random_test_hold(uint64_t seed) {
    uint32_t f = spin_lock(&lock);
    saved = gen;
    saved_file_bits = file_bits;   /* a test's credits are not the boot's */
    held = 1;
    fortuna_init(&gen);
    if (seed) fortuna_add(&gen, &seed, sizeof seed, FORTUNA_SEED_BITS);
    spin_unlock(&lock, f);
}

void random_test_release(void) {
    uint32_t f = spin_lock(&lock);
    gen = saved;
    file_bits = saved_file_bits;
    held = 0;
    for (uint32_t i = 0; i < sizeof saved; i++) ((volatile uint8_t *)&saved)[i] = 0;
    spin_unlock(&lock, f);
}
#endif
