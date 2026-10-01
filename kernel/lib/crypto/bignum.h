/* /kernel/lib/bignum.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Fixed-size big integers for RSA-2048 signature VERIFICATION only
 * (CX_EXTENSION_SYSTEM.md section 10). Just the operations modular
 * exponentiation needs: load/store big-endian bytes, compare, multiply, modulo.
 *
 * Schoolbook arithmetic - chosen deliberately: verification runs only a handful
 * of times (kernel/driver load), so speed is irrelevant, and simplicity keeps
 * this security-critical code correct and auditable. No Montgomery, no signing.
 *
 * Representation: little-endian array of 32-bit limbs (limb[0] = least
 * significant). Sized to hold a full 4096-bit product (2*2048) plus headroom,
 * so multiply never overflows the array.
 */

#ifndef BIGNUM_H
#define BIGNUM_H

#include <stdint.h>
#include <stddef.h>

#define BN_BITS   2048
#define BN_LIMBS  (BN_BITS / 32)        /* 64 limbs for a 2048-bit value */
#define BN_MAXLIMBS (BN_LIMBS * 2 + 2)  /* room for products (4096-bit) + carry */

typedef struct {
    uint32_t limb[BN_MAXLIMBS];         /* little-endian limbs */
    int      used;                       /* number of significant limbs */
} bignum;

/* zero a bignum */
void bn_zero(bignum *a);

/* load a big-endian byte string into a bignum (e.g. RSA modulus / signature) */
void bn_from_bytes(bignum *a, const uint8_t *bytes, size_t len);

/* store a bignum into a big-endian byte buffer of exactly `len` bytes
   (left-zero-padded). returns 0 on success, -1 if the value doesn't fit. */
int  bn_to_bytes(const bignum *a, uint8_t *out, size_t len);

/* compare: returns -1 if a<b, 0 if a==b, 1 if a>b */
int  bn_cmp(const bignum *a, const bignum *b);

/* r = a * b   (schoolbook). r must not alias a or b. */
void bn_mul(bignum *r, const bignum *a, const bignum *b);

/* r = a mod m   (schoolbook bit-shift division). r may alias a. */
void bn_mod(bignum *r, const bignum *a, const bignum *m);

/* result = base^exp mod m   (square-and-multiply). The only public entry the
   RSA verifier needs. exp is a small public exponent (e.g. 65537). */
void bn_modexp(bignum *result, const bignum *base, uint32_t exp, const bignum *m);

#endif