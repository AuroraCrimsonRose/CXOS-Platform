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

#define RING          128      /* samples between drains; more are dropped */
#define JITTER_MAX    8192     /* boot loop rounds at most */
#define CPU_WORDS     4        /* 64-bit words asked of RDSEED / RDRAND */
#define CPU_TRIES     10       /* RDSEED may run dry for a moment */

struct sample { uint32_t tsc, vector; };

static struct sample ring[RING];
static volatile uint32_t ring_n;
static uint32_t taken, dropped;

static spinlock_t lock = SPINLOCK_INIT;
static struct fortuna gen;
static struct jitter_est irq_est;
static uint32_t cpu_bits, jitter_bits, irq_bits;

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

#if CXK_TEST_HOOKS
void random_test_hold(uint64_t seed) {
    uint32_t f = spin_lock(&lock);
    saved = gen;
    held = 1;
    fortuna_init(&gen);
    if (seed) fortuna_add(&gen, &seed, sizeof seed, FORTUNA_SEED_BITS);
    spin_unlock(&lock, f);
}

void random_test_release(void) {
    uint32_t f = spin_lock(&lock);
    gen = saved;
    held = 0;
    for (uint32_t i = 0; i < sizeof saved; i++) ((volatile uint8_t *)&saved)[i] = 0;
    spin_unlock(&lock, f);
}
#endif
