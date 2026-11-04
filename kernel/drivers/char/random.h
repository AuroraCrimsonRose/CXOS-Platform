// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/char/random.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The kernel's random numbers (docs/security/CX_RANDOM.md): the generator of
 * lib/crypto/fortuna.c, fed by
 *   - RDSEED, else RDRAND, at boot, where the CPU has them (64 bits a
 *     plausible word; the rig's FX-8350 has neither),
 *   - timing jitter at boot: the cycle counter around a loop of memory reads,
 *   - the cycle counter at every interrupt, after.
 * An interrupt only stores its sample (XNU's entropy_collect does the same,
 * osfmk/prng/entropy.c); the samples are hashed in when someone asks for
 * bytes, under the generator's lock, never in the interrupt.
 *
 * Nothing is handed out before 256 bits have been credited: there is no
 * fallback seed (Serenity's, from the RTC, is the warning:
 * Kernel/Security/Random.cpp). Nothing blocks either: until then the answer
 * is E_SHOULD_WAIT.
 *
 * One CPU: the sample ring is written with interrupts off and drained with
 * them off. SMP gives each CPU its own ring (roadmap step 9).
 */

#ifndef CXK_RANDOM_H
#define CXK_RANDOM_H

#include <stdint.h>
#include "config.h"

/* Once, after the TSC is calibrated (kmain): the boot seed. */
void random_init(void);

/* From irq_dispatch, every interrupt: store the cycle counter. */
void random_irq_sample(uint32_t vector);

/* `len` bytes into `out`: 0, or E_SHOULD_WAIT until seeded (nothing
   written). Any context but an interrupt handler. */
int random_bytes(void *out, uint32_t len);

int random_seeded(void);

struct random_stats {
    uint32_t seeded;
    uint32_t cpu_bits;        /* credited at boot from RDSEED / RDRAND */
    uint32_t jitter_bits;     /* credited at boot from the jitter loop */
    uint32_t irq_bits;        /* credited from interrupts, before seeding */
    uint32_t taken;           /* interrupt samples stored */
    uint32_t dropped;         /* ... and dropped, the ring full */
    uint32_t pending;         /* stored and not yet hashed in */
    uint64_t reseeds;
};
void random_stats(struct random_stats *s);

#if CXK_TEST_HOOKS
/* For the self-tests: set the generator aside and run a fresh one, unseeded
   (seed == 0) or seeded from `seed` alone (fortuna_add of its 8 bytes,
   credited in full), until random_test_release puts the real one back.
   Interrupt samples wait in the ring meanwhile. */
void random_test_hold(uint64_t seed);
void random_test_release(void);
#endif

#endif
