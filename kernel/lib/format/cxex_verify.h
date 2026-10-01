/* /kernel/lib/format/cxex_verify.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXEX signature verification (CX_EXTENSION_SYSTEM.md section 10), the on-device
 * half of code signing. Ties together the format layer (cxex.c: locate the CXSG
 * block + signed region), the hash (sha256), and RSA verification (rsa.c) into
 * one call the loader uses before placing an image in memory.
 *
 * Verification proves two separate things:
 *   IDENTITY   - the CXSG key fingerprint matches a key we trust (sha256 of the
 *                trusted .xkpk). Answers "who signed this".
 *   INTEGRITY  - the RSA signature over the image [0, signature_offset) is valid.
 *                Answers "is it intact / unforged".
 * It does NOT decide what the image is ALLOWED to do - that is a separate policy
 * step (identity in, authorization out).
 */

#ifndef CXEX_VERIFY_H
#define CXEX_VERIFY_H

#include <stdint.h>
#include <stddef.h>

enum cxex_verify_result {
    CXEX_VERIFY_OK            =  0,
    CXEX_VERIFY_BAD_FORMAT    = -1,   /* header didn't parse / bad magic */
    CXEX_VERIFY_UNSIGNED      = -2,   /* no SIGNED flag or no signature block */
    CXEX_VERIFY_BAD_SIG_BLOCK = -3,   /* CXSG missing, truncated, or OOB */
    CXEX_VERIFY_BAD_ALGO      = -4,   /* not RSA-2048 / SHA-256 */
    CXEX_VERIFY_BAD_KEY       = -5,   /* trusted .xkpk failed to parse */
    CXEX_VERIFY_WRONG_KEY     = -6,   /* signed by a key we don't trust */
    CXEX_VERIFY_BAD_SIG_LEN   = -7,   /* signature length != key modulus length */
    CXEX_VERIFY_BAD_SIGNATURE = -8    /* RSA verify failed: tampered or forged */
};

/* Verify `file` (a complete .xkex/.xuex image incl. the appended CXSG block)
   against the trusted public key `trusted_xkpk` (CXPK format, as makekeys.py
   writes). Returns CXEX_VERIFY_OK (0) or a negative cxex_verify_result. */
int cxex_verify(const uint8_t *file, size_t len,
                const uint8_t *trusted_xkpk, size_t xkpk_len);

/* Verify against the kernel's compiled-in root of trust (trusted_key.c, baked
   from the system .xkpk by embedkey.py). This is the call the loader uses; the
   trusted key is NOT read from disk. Same return values as cxex_verify. */
int cxex_verify_trusted(const uint8_t *file, size_t len);

/* INTEGRITY ONLY, against the key the image itself carries.
 *
 * Answers "are these bytes exactly what whoever signed them signed" and
 * nothing about who that was. An image from a signer nobody here has heard of
 * still passes this, which is the point: without it, "unknown publisher" and
 * "tampered with" would be the same answer, and a trust decision made on that
 * basis would be made blind. Returns CXEX_VERIFY_OK or a negative result. */
int cxex_verify_self(const uint8_t *file, size_t len);

/* The signer's .xkpk, carried inside the image, or NULL if the image does not
   verify against it. The bytes live in `file` and are valid as long as it is.
   Use it to ask WHO signed something once cxex_verify_self has established
   that the signature holds. */
const uint8_t *cxex_signer_key(const uint8_t *file, size_t len, uint16_t *out_len);

/* Human-readable message for a cxex_verify result (for logging). */
const char *cxex_verify_strerror(int result);

#endif