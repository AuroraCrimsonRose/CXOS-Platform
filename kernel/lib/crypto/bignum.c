// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/bignum.c */
/* Aurora Tejeda / CATX Systems */
/* Fixed-size schoolbook big integers for RSA-2048 verification. */

#include "bignum.h"
#ifndef CXK_HOST_TEST
#include "arch/cpu_ops.h"   /* arch_irqs_enabled */
#include "arch/smp.h"       /* smp_serve: a remainder answers shootdowns */
#define BN_SERVE() do { if (!arch_irqs_enabled()) smp_serve(); } while (0)
#else
#define BN_SERVE() do { } while (0)
#endif

/* recompute the `used` count (index+1 of the highest non-zero limb) */
static void bn_normalize(bignum *a) {
    int i = BN_MAXLIMBS - 1;
    while (i > 0 && a->limb[i] == 0) i--;
    a->used = (a->limb[i] == 0) ? 0 : (i + 1);
}

void bn_zero(bignum *a) {
    for (int i = 0; i < BN_MAXLIMBS; i++) a->limb[i] = 0;
    a->used = 0;
}

void bn_from_bytes(bignum *a, const uint8_t *bytes, size_t len) {
    bn_zero(a);
    /* big-endian input: last byte is least significant. Walk from the end. */
    int limb_i = 0, shift = 0;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (limb_i >= BN_MAXLIMBS) break;             /* defensive: overflow */
        a->limb[limb_i] |= ((uint32_t)bytes[i]) << shift;
        shift += 8;
        if (shift == 32) { shift = 0; limb_i++; }
    }
    bn_normalize(a);
}

int bn_to_bytes(const bignum *a, uint8_t *out, size_t len) {
    /* big-endian output, left-zero-padded to `len`. fail if value doesn't fit */
    for (size_t i = 0; i < len; i++) out[i] = 0;
    for (int li = 0; li < BN_MAXLIMBS; li++) {
        uint32_t v = a->limb[li];
        for (int b = 0; b < 4; b++) {
            uint8_t byte = (uint8_t)(v >> (b * 8));
            int byte_pos = li * 4 + b;                 /* from the LS end */
            if (byte == 0) continue;
            if ((size_t)byte_pos >= len) return -1;     /* doesn't fit */
            out[len - 1 - byte_pos] = byte;
        }
    }
    return 0;
}

int bn_cmp(const bignum *a, const bignum *b) {
    /* compare by scanning from the most significant limb down */
    for (int i = BN_MAXLIMBS - 1; i >= 0; i--) {
        if (a->limb[i] != b->limb[i])
            return (a->limb[i] > b->limb[i]) ? 1 : -1;
    }
    return 0;
}

void bn_mul(bignum *r, const bignum *a, const bignum *b) {
    bn_zero(r);
    int an = (a->used > 0) ? a->used : 1;
    int bn = (b->used > 0) ? b->used : 1;
    for (int i = 0; i < an; i++) {
        uint64_t carry = 0;
        uint64_t ai = a->limb[i];
        for (int j = 0; j < bn; j++) {
            if (i + j >= BN_MAXLIMBS) break;
            uint64_t cur = (uint64_t)r->limb[i + j]
                         + ai * (uint64_t)b->limb[j]
                         + carry;
            r->limb[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        if (i + bn < BN_MAXLIMBS)
            r->limb[i + bn] += (uint32_t)carry;
    }
    bn_normalize(r);
}

/* The remainder, a word at a time: Knuth's Algorithm D (TAOCP vol. 2,
   4.3.1), as Serenity's divide_without_allocation does it. Each step guesses
   one 32-bit quotient digit from the top two words, corrects the guess (at
   most twice) from the third, and subtracts that multiple of m; a guess one
   too large is put right by adding m back. About 64 x 64 word operations a
   remainder, where the bit-serial one it replaced made 4096 passes over the
   whole number (~30 s guest a signature check, 2.8a'4b).

   m == 0 gives 0. r may alias a. */
void bn_mod(bignum *r, const bignum *a, const bignum *m) {
    int n = m->used, al = a->used;
    if (n == 0) { bn_zero(r); return; }
    if (bn_cmp(a, m) < 0) { if (r != a) *r = *a; return; }

    if (n == 1) {                       /* one-word divisor: a running remainder */
        uint64_t d = m->limb[0], rem = 0;
        for (int i = al - 1; i >= 0; i--) rem = ((rem << 32) | a->limb[i]) % d;
        bn_zero(r);
        r->limb[0] = (uint32_t)rem;
        bn_normalize(r);
        return;
    }

    /* Normalise: shift both left until m's top word has its top bit set,
       which keeps each guess within 2 of the true digit. u has one word
       more than a for the shifted-out bits. */
    uint32_t u[BN_MAXLIMBS + 1], v[BN_MAXLIMBS];
    int s = __builtin_clz(m->limb[n - 1]);
    for (int i = n - 1; i > 0; i--)
        v[i] = (m->limb[i] << s) | (s ? m->limb[i - 1] >> (32 - s) : 0);
    v[0] = m->limb[0] << s;
    u[al] = s ? a->limb[al - 1] >> (32 - s) : 0;
    for (int i = al - 1; i > 0; i--)
        u[i] = (a->limb[i] << s) | (s ? a->limb[i - 1] >> (32 - s) : 0);
    u[0] = a->limb[0] << s;

    for (int j = al - n; j >= 0; j--) {
        uint64_t top = ((uint64_t)u[j + n] << 32) | u[j + n - 1];
        uint64_t qhat = top / v[n - 1], rhat = top % v[n - 1];
        while (qhat >> 32 ||
               qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
            qhat--;
            rhat += v[n - 1];
            if (rhat >> 32) break;
        }
        /* u[j..j+n] -= qhat * v */
        uint64_t carry = 0, borrow = 0;
        for (int i = 0; i < n; i++) {
            uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            uint64_t t = (uint64_t)u[i + j] - (uint32_t)p - borrow;
            u[i + j] = (uint32_t)t;
            borrow = t >> 63;
        }
        uint64_t t = (uint64_t)u[j + n] - carry - borrow;
        u[j + n] = (uint32_t)t;
        if (t >> 63) {                  /* the guess was one too large: add m back */
            uint64_t c = 0;
            for (int i = 0; i < n; i++) {
                c += (uint64_t)u[i + j] + v[i];
                u[i + j] = (uint32_t)c;
                c >>= 32;
            }
            u[j + n] += (uint32_t)c;
        }
        /* An image's signature is checked inside a syscall, interrupts off:
           a remainder serves an AP's shootdown as a spin does (1.9e4b), so a
           run of them can't hold one past its timeout. */
        BN_SERVE();
    }

    /* The remainder is u[0..n-1], shifted back. */
    bn_zero(r);
    for (int i = 0; i < n - 1; i++)
        r->limb[i] = (u[i] >> s) | (s ? u[i + 1] << (32 - s) : 0);
    r->limb[n - 1] = u[n - 1] >> s;
    bn_normalize(r);
}

void bn_modexp(bignum *result, const bignum *base, uint32_t exp, const bignum *m) {
    /* square-and-multiply. result = 1; for each bit of exp (LSB->MSB):
         if bit set: result = result*base mod m
         base = base*base mod m
       exp is the small public exponent, so only ~17 iterations for 65537. */
    bignum r, b, tmp;
    bn_zero(&r);
    r.limb[0] = 1; r.used = 1;          /* r = 1 */
    bn_mod(&b, base, m);                /* b = base mod m */

    while (exp > 0) {
        if (exp & 1u) {
            bn_mul(&tmp, &r, &b);
            bn_mod(&r, &tmp, m);
        }
        exp >>= 1;
        if (exp > 0) {
            bn_mul(&tmp, &b, &b);
            bn_mod(&b, &tmp, m);
        }
    }
    *result = r;
}