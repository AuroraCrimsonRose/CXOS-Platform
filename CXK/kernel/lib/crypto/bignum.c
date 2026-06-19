/* /CXK/kernel/lib/bignum.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* Fixed-size schoolbook big integers for RSA-2048 verification. */

#include "bignum.h"

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

/* test bit `n` of a (0 = least significant) */
static int bn_test_bit(const bignum *a, int n) {
    int limb = n >> 5, bit = n & 31;
    if (limb >= BN_MAXLIMBS) return 0;
    return (a->limb[limb] >> bit) & 1u;
}

/* highest set bit index, or -1 if zero */
static int bn_bitlen(const bignum *a) {
    for (int i = BN_MAXLIMBS - 1; i >= 0; i--) {
        if (a->limb[i]) {
            uint32_t v = a->limb[i];
            int b = 31;
            while (b > 0 && !((v >> b) & 1u)) b--;
            return i * 32 + b;
        }
    }
    return -1;
}

/* r <<= 1 (in place) */
static void bn_shl1(bignum *a) {
    uint32_t carry = 0;
    for (int i = 0; i < BN_MAXLIMBS; i++) {
        uint32_t newcarry = a->limb[i] >> 31;
        a->limb[i] = (a->limb[i] << 1) | carry;
        carry = newcarry;
    }
    bn_normalize(a);
}

/* r -= b   (assumes r >= b). */
static void bn_sub(bignum *r, const bignum *b) {
    uint64_t borrow = 0;
    for (int i = 0; i < BN_MAXLIMBS; i++) {
        uint64_t cur = (uint64_t)r->limb[i] - b->limb[i] - borrow;
        r->limb[i] = (uint32_t)cur;
        borrow = (cur >> 63) & 1u;   /* 1 if it wrapped (negative) */
    }
    bn_normalize(r);
}

void bn_mod(bignum *r, const bignum *a, const bignum *m) {
    /* schoolbook bit-shift remainder: walk bits of a from high to low,
       building rem; subtract m whenever rem >= m. Correct and simple. */
    bignum rem;
    bn_zero(&rem);

    int top = bn_bitlen(a);
    if (top < 0) { *r = rem; return; }   /* a == 0 */

    for (int i = top; i >= 0; i--) {
        bn_shl1(&rem);
        if (bn_test_bit(a, i)) rem.limb[0] |= 1u;
        rem.used = (rem.used > 0) ? rem.used : 1;
        if (bn_cmp(&rem, m) >= 0) bn_sub(&rem, m);
    }
    bn_normalize(&rem);
    *r = rem;
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