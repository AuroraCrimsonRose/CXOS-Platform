/* /CXK/kernel/lib/format/cxex_verify.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* CXEX signature verification. See cxex_verify.h. */

#include "cxex_verify.h"
#include "cxex.h"
#include "sha256.h"
#include "rsa.h"
#include "trusted_key.h"

/* algorithm IDs the signer (signcxex.py) writes into the CXSG block */
#define SIG_ALGO_RSA2048_SHA256  1
#define HASH_ALGO_SHA256         1

static int eq32(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 32; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int cxex_verify(const uint8_t *file, size_t len,
                const uint8_t *trusted_xkpk, size_t xkpk_len) {
    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_VERIFY_BAD_FORMAT;
    if (!cxex_is_signed(&h))                   return CXEX_VERIFY_UNSIGNED;

    struct cxex_sig sig;
    if (cxex_get_sig(file, len, &h, &sig) != 0) return CXEX_VERIFY_BAD_SIG_BLOCK;
    if (sig.sig_algo  != SIG_ALGO_RSA2048_SHA256 ||
        sig.hash_algo != HASH_ALGO_SHA256)      return CXEX_VERIFY_BAD_ALGO;

    /* IDENTITY: the CXSG fingerprint is sha256(the signer's .xkpk). It must equal
       the fingerprint of the key WE trust, or this was signed by someone else. */
    uint8_t fp[32];
    sha256(trusted_xkpk, xkpk_len, fp);
    if (!eq32(fp, sig.fingerprint))            return CXEX_VERIFY_WRONG_KEY;

    struct rsa_pubkey key;
    if (rsa_parse_xkpk(trusted_xkpk, xkpk_len, &key) != 0) return CXEX_VERIFY_BAD_KEY;
    if (sig.sig_len != key.modulus_len)        return CXEX_VERIFY_BAD_SIG_LEN;

    /* INTEGRITY: hash exactly the bytes that were signed - the image up to the
       signature block, [0, signature_offset) - and RSA-verify the signature. */
    uint8_t digest[32];
    sha256(file, h.signature_offset, digest);

    if (rsa_verify_sha256(&key, file + sig.sig_file_offset, sig.sig_len, digest) != 1)
        return CXEX_VERIFY_BAD_SIGNATURE;

    return CXEX_VERIFY_OK;
}

int cxex_verify_trusted(const uint8_t *file, size_t len) {
    return cxex_verify(file, len, cxos_trusted_key, cxos_trusted_key_len);
}

const char *cxex_verify_strerror(int r) {
    switch (r) {
        case CXEX_VERIFY_OK:            return "ok";
        case CXEX_VERIFY_BAD_FORMAT:    return "bad CXEX header";
        case CXEX_VERIFY_UNSIGNED:      return "image is not signed";
        case CXEX_VERIFY_BAD_SIG_BLOCK: return "malformed CXSG block";
        case CXEX_VERIFY_BAD_ALGO:      return "unsupported sig/hash algorithm";
        case CXEX_VERIFY_BAD_KEY:       return "bad trusted public key";
        case CXEX_VERIFY_WRONG_KEY:     return "signed by an untrusted key";
        case CXEX_VERIFY_BAD_SIG_LEN:   return "signature length mismatch";
        case CXEX_VERIFY_BAD_SIGNATURE: return "signature verification failed";
        default:                        return "unknown error";
    }
}