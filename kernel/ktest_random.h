// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_random.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The kernel's random numbers (roadmap 1.7b, drivers/char/random.h). The
 * generator's own rules are host tests (kernel/test/host/test_random.c);
 * these are the parts only a boot has: the cipher as the kernel was built,
 * the interrupt path, the syscall from ring 3, and the boot seed.
 */

#ifndef KTEST_RANDOM_H
#define KTEST_RANDOM_H

/* chacha20_block, built as the kernel is, gives RFC 8439's section 2.3.2
   block. */
int ktest_random_chacha(void);

/* Interrupts store samples while the test sleeps, and a request hashes
   every stored one in. */
int ktest_random_interrupts(void);

/* SYS_RANDOM_READ from ring 3: refused (E_SHOULD_WAIT, nothing written) by
   an unseeded generator; from a seeded one, exactly the generator's bytes,
   each read different; E_OUT_OF_RANGE past RANDOM_READ_MAX, 0 for 0 bytes,
   E_BAD_ADDRESS for a kernel address without a fault; the real generator
   back afterwards. */
int ktest_random_syscall(void);

/* The seed file (crash area): a seed whose rewrite reads back is credited
   in full and the file ends with the generator's bytes; one whose rewrite
   the disk drops (RP_SEED_WRITTEN) is not credited; a damaged one neither,
   and is left alone unseeded; seeded, the file takes the generator's bytes.
   Leaves the real generator's seed in the file. */
int ktest_random_seed_file(void);

/* A CPU with RDSEED or RDRAND seeded the generator at boot from its words
   (256 bits or more credited); one with neither credited none. */
int ktest_random_boot_seed(void);

#endif
