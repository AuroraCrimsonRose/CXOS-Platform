/* /kernel/lib/rsa.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* RSA-2048 PKCS#1 v1.5 / SHA-256 signature verification. */

#include "rsa.h"
#include "bignum.h"

/* DigestInfo ASN.1 prefix for SHA-256 (precedes the 32-byte hash in a
   PKCS#1 v1.5 signature block). */
static const uint8_t SHA256_DIGESTINFO[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

/* read a little-endian uint from bytes (the .xkpk header is little-endian) */
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rsa_parse_xkpk(const uint8_t *file, size_t len, struct rsa_pubkey *out) {
    /* header is 16 bytes: magic(4) version(2) key_bits(2) exp(4) mod_len(2) resv(2) */
    if (len < 16) return -1;
    if (!(file[0]=='C' && file[1]=='X' && file[2]=='P' && file[3]=='K')) return -1;
    /* version = rd16(file+4); key_bits = rd16(file+6); (not strictly needed) */
    uint32_t exp = rd32(file + 8);
    uint32_t mod_len = rd16(file + 12);

    if (mod_len == 0 || mod_len > sizeof(out->modulus)) return -1;
    if (len < 16 + mod_len) return -1;

    for (uint32_t i = 0; i < mod_len; i++) out->modulus[i] = file[16 + i];
    out->modulus_len = mod_len;
    out->exponent = exp;
    return 0;
}

int rsa_verify_sha256(const struct rsa_pubkey *key,
                      const uint8_t *sig, size_t sig_len,
                      const uint8_t digest[32]) {
    if (sig_len != key->modulus_len) return 0;
    if (key->modulus_len > 256) return 0;

    /* 1. m = sig^e mod n  (the RSA verify operation) */
    bignum s, n, m;
    bn_from_bytes(&s, sig, sig_len);
    bn_from_bytes(&n, key->modulus, key->modulus_len);

    /* signature must be < modulus to be valid */
    if (bn_cmp(&s, &n) >= 0) return 0;

    bn_modexp(&m, &s, key->exponent, &n);

    /* serialize the recovered value back to a modulus_len-wide big-endian block */
    uint8_t em[256];
    if (bn_to_bytes(&m, em, key->modulus_len) != 0) return 0;

    /* 2. check PKCS#1 v1.5 structure:
          em = 0x00 0x01 [0xFF * k] 0x00 [19-byte DigestInfo] [32-byte hash]
       (k >= 8 per the standard). */
    size_t L = key->modulus_len;
    const size_t tail = 19 + 32;          /* DigestInfo + hash */
    if (L < 11 + tail) return 0;          /* must be large enough */

    if (em[0] != 0x00) return 0;
    if (em[1] != 0x01) return 0;

    /* scan the 0xFF padding */
    size_t i = 2;
    size_t pad = 0;
    while (i < L && em[i] == 0xFF) { i++; pad++; }
    if (pad < 8) return 0;                /* minimum padding length */
    if (i >= L || em[i] != 0x00) return 0;/* separator */
    i++;

    /* what remains must be exactly DigestInfo + the 32-byte hash */
    if (L - i != tail) return 0;

    for (int j = 0; j < 19; j++)
        if (em[i + j] != SHA256_DIGESTINFO[j]) return 0;

    /* 3. compare the embedded hash to the caller's digest */
    const uint8_t *embedded = em + i + 19;
    int diff = 0;
    for (int j = 0; j < 32; j++) diff |= (embedded[j] ^ digest[j]);
    return diff == 0 ? 1 : 0;
}