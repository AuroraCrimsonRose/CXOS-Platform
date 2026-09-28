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

/* INTEGRITY, against the key the IMAGE carries.
 *
 * This proves the bytes are exactly what whoever signed them signed, and says
 * nothing whatever about who that was. Splitting it out is the whole point of
 * carrying the key: an image from a signer we have never heard of can still be
 * shown to be intact, so "we do not know this publisher" and "this file has
 * been tampered with" stop being the same answer.
 *
 * On success `*h` and `*sig` are filled in for the caller's identity check.
 */
static int verify_self(const uint8_t *file, size_t len,
                       struct cxex_header *h, struct cxex_sig *sig) {
    if (cxex_parse_header(file, len, h) != 0)  return CXEX_VERIFY_BAD_FORMAT;
    if (!cxex_is_signed(h))                    return CXEX_VERIFY_UNSIGNED;
    if (cxex_get_sig(file, len, h, sig) != 0)  return CXEX_VERIFY_BAD_SIG_BLOCK;
    if (sig->sig_algo  != SIG_ALGO_RSA2048_SHA256 ||
        sig->hash_algo != HASH_ALGO_SHA256)    return CXEX_VERIFY_BAD_ALGO;
    if (sig->pubkey_len == 0)                  return CXEX_VERIFY_BAD_KEY;

    const uint8_t *pk = file + sig->pubkey_file_offset;

    /* The fingerprint must describe the key actually carried. It is only an
       identifier and nothing is decided by it, but a lying one would put a
       trusted publisher's fingerprint in a log line next to an image that key
       never signed - so the block has to be self-consistent before it is
       allowed to name anybody. */
    uint8_t fp[32];
    sha256(pk, sig->pubkey_len, fp);
    if (!eq32(fp, sig->fingerprint))           return CXEX_VERIFY_BAD_KEY;

    struct rsa_pubkey key;
    if (rsa_parse_xkpk(pk, sig->pubkey_len, &key) != 0) return CXEX_VERIFY_BAD_KEY;
    if (sig->sig_len != key.modulus_len)       return CXEX_VERIFY_BAD_SIG_LEN;

    /* Hash exactly the bytes that were signed - the image up to the signature
       block, [0, signature_offset) - and RSA-verify against the carried key. */
    uint8_t digest[32];
    sha256(file, h->signature_offset, digest);
    if (rsa_verify_sha256(&key, file + sig->sig_file_offset, sig->sig_len, digest) != 1)
        return CXEX_VERIFY_BAD_SIGNATURE;

    return CXEX_VERIFY_OK;
}

int cxex_verify_self(const uint8_t *file, size_t len) {
    struct cxex_header h;
    struct cxex_sig sig;
    return verify_self(file, len, &h, &sig);
}

const uint8_t *cxex_signer_key(const uint8_t *file, size_t len, uint16_t *out_len) {
    struct cxex_header h;
    struct cxex_sig sig;
    if (verify_self(file, len, &h, &sig) != CXEX_VERIFY_OK) return 0;
    if (out_len) *out_len = sig.pubkey_len;
    return file + sig.pubkey_file_offset;
}

int cxex_verify(const uint8_t *file, size_t len,
                const uint8_t *trusted_xkpk, size_t xkpk_len) {
    struct cxex_header h;
    struct cxex_sig sig;
    int rc = verify_self(file, len, &h, &sig);
    if (rc != CXEX_VERIFY_OK) return rc;

    /* IDENTITY: the carried key must BE the key we trust.
     *
     * Compared as bytes rather than by fingerprint on purpose. The fingerprint
     * is the image's own claim about itself; the key is the thing the
     * signature was actually checked against a moment ago. Trust has to hang
     * off the second, or an image could be believed on the strength of a
     * 32-byte field it wrote itself. */
    if ((size_t)sig.pubkey_len != xkpk_len)    return CXEX_VERIFY_WRONG_KEY;
    const uint8_t *pk = file + sig.pubkey_file_offset;
    for (size_t i = 0; i < xkpk_len; i++)
        if (pk[i] != trusted_xkpk[i])          return CXEX_VERIFY_WRONG_KEY;

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