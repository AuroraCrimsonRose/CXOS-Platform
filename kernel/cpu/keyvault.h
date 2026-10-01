/* /kernel/cpu/keyvault.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * The Key Vault - which signing keys this machine believes, and how much.
 *
 * This is the POLICY half of code signing. The format layer (cxex_verify.c)
 * answers two questions and stops: are these bytes intact, and whose key
 * signed them. It deliberately knows nothing about filesystems or about what
 * any of it is allowed to do. This file is where identity becomes authority.
 *
 * Two kinds of key, and the extension says which:
 *
 *   .xkpk   the PLATFORM root, compiled into the kernel (trusted_key.c). It
 *           is the only key that may sign a .xkex, .xbex, .xoex or .xsex -
 *           anything the operating system itself is made of.
 *   .xupk   a PUBLISHER's, kept in /System/KeyVault on disk. It may sign a
 *           .xuex and nothing else.
 *
 * A key's extension therefore states its ceiling. A .xupk in the vault can
 * never authorise a system program, and you can see that from the filename
 * without opening it or consulting a table.
 */

#ifndef KEYVAULT_H
#define KEYVAULT_H

#include <stdint.h>
#include <stddef.h>

/* Where publisher keys live. SYSTEM-owned 0755, so only SYSTEM writes it -
   adding a key here is the act of deciding to believe a publisher. */
#define KEYVAULT_DIR      "/System/KeyVault"
#define KEYVAULT_MAX_KEY  1024      /* an RSA-2048 .xkpk is ~270 bytes */

/* How far a signer is trusted, once integrity has already been established.
   Ordered so higher is more trusted and a policy reads as a minimum:
   "a .xsex needs at least CX_TRUST_PLATFORM". */
enum cx_trust {
    CX_TRUST_UNVERIFIED = 0,   /* valid signature, key we do not know */
    CX_TRUST_PUBLISHER  = 1,   /* key is in the vault */
    CX_TRUST_PLATFORM   = 2    /* the key baked into this kernel */
};

/* Establish integrity against the image's own carried key, then say how far
   its signer is trusted.

   Returns a cx_trust level, or a NEGATIVE cxex_verify_result if the image does
   not verify at all - including CXEX_VERIFY_UNSIGNED for one with no signature
   block. Callers must tell the two apart by sign: "unsigned" and "signed by
   someone we do not know" are different facts and deserve different answers. */
int keyvault_trust_of(const uint8_t *file, size_t len);

/* Name of a trust level, for logging. */
const char *keyvault_trust_name(int level);

#endif
