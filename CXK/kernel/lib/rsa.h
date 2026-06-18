/* /CXK/kernel/lib/rsa.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * RSA-2048 signature VERIFICATION (CX_EXTENSION_SYSTEM.md section 10).
 * Verifies a PKCS#1 v1.5 signature over a SHA-256 digest, using a public key.
 *
 * The kernel only ever VERIFIES (the cheap direction: sig^65537 mod n). Signing
 * and key generation happen offline on the build machine (signcxex.py /
 * makekeys.py). This is the on-device half of code signing.
 */

#ifndef RSA_H
#define RSA_H

#include <stdint.h>
#include <stddef.h>

/* an RSA public key: modulus (big-endian bytes) + exponent. */
struct rsa_pubkey {
    uint8_t  modulus[256];   /* RSA-2048 modulus, big-endian */
    uint32_t modulus_len;    /* 256 for RSA-2048 */
    uint32_t exponent;       /* public exponent, e.g. 65537 */
};

/* Parse a .xkpk public-key file (the format makekeys.py writes) into a pubkey.
   Layout (little-endian, no padding - matches Python struct "<4sHHIHH"):
     off 0:  "CXPK"  (4)
     off 4:  version (2)
     off 6:  key_bits(2)
     off 8:  exponent(4)
     off 12: modulus_len(2)
     off 14: reserved(2)
     off 16: modulus_len bytes of big-endian modulus
   Returns 0 on success, -1 on bad magic / size. */
int rsa_parse_xkpk(const uint8_t *file, size_t len, struct rsa_pubkey *out);

/* Verify a PKCS#1 v1.5 / SHA-256 signature.
   - key:        the public key
   - sig:        signature bytes (modulus_len long, big-endian)
   - sig_len:    must equal key->modulus_len
   - digest:     the SHA-256 (32 bytes) of the data that was supposedly signed
   Returns 1 if the signature is valid for that digest, 0 otherwise. */
int rsa_verify_sha256(const struct rsa_pubkey *key,
                      const uint8_t *sig, size_t sig_len,
                      const uint8_t digest[32]);

#endif