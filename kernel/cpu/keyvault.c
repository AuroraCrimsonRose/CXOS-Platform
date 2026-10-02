/* /kernel/cpu/keyvault.c */
/* Aurora Tejeda / CATX Systems */
/* The Key Vault. See keyvault.h. */

#include "keyvault.h"
#include "cxex_verify.h"
#include "trusted_key.h"
#include "cxfs.h"
#include "logging.h"

/* One staging buffer for a key read off the disk.
 *
 * Static for the same reason every other CXFS buffer in this kernel is: a
 * kilobyte on an 8KB thread stack is a large fraction of it, and this runs
 * underneath exec_path, which is already several frames deep. Safe because
 * CXFS is entered serially - see the note in cxfs.c. */
static uint8_t vault_key[KEYVAULT_MAX_KEY];

/* Byte equality. Not constant-time, and does not need to be: both sides are
   PUBLIC keys, and the answer is already visible to anyone who can read the
   image or list the vault. There is no secret here to leak through timing. */
static int same_bytes(const uint8_t *a, size_t alen,
                      const uint8_t *b, size_t blen) {
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* cxfs_list_dir hands entries to a callback rather than taking an index, so
   the key being looked for and the answer live here across the walk. Same
   pattern sysfile.c's readdir uses, and safe for the same reason: CXFS is
   entered serially, so two walks are never in flight at once. */
static const uint8_t *want_key;
static uint16_t       want_len;
static int            vault_hit;

static void vault_cb(const struct cxfs_entry *e) {
    if (vault_hit) return;                              /* already found it */
    if (e->type != CXFS_TYPE_FILE) return;
    if (e->size == 0 || e->size > KEYVAULT_MAX_KEY) return;

    /* Reading a file from inside a directory walk is safe here: list_dir
       stages through dir_blk and read_file through dat_blk, which is exactly
       what the per-role buffers in cxfs.c are for. */
    uint32_t n = (uint32_t)e->size;
    if (cxfs_read_file(e->id, vault_key, n) != (int)n) return;
    if (same_bytes(want_key, want_len, vault_key, (size_t)n)) vault_hit = 1;
}

/* Is `key` one of the publisher keys in the vault?
 *
 * Read from disk on every launch rather than cached at boot. The vault is
 * small and this runs once per program start, and a cache would mean deciding
 * when to invalidate it - so a key added to the vault takes effect on the next
 * launch with nothing to flush, and a key REMOVED stops being believed just as
 * promptly. The second one matters more. */
static int in_vault(const uint8_t *key, uint16_t keylen) {
    if (!cxfs_is_mounted()) return 0;

    int dir = cxfs_resolve(KEYVAULT_DIR, 0);
    if (dir < 0) return 0;                  /* no vault: no publishers */

    want_key = key;
    want_len = keylen;
    vault_hit = 0;
    cxfs_list_dir((uint32_t)dir, vault_cb);
    want_key = 0;
    return vault_hit;
}

int keyvault_trust_of(const uint8_t *file, size_t len) {
    /* INTEGRITY first, always, and against the key the image itself carries.
       Nothing below this line is worth asking until the bytes are known to be
       what the signer signed. */
    int rc = cxex_verify_self(file, len);
    if (rc != CXEX_VERIFY_OK) return rc;    /* negative: unsigned, tampered, ... */

    uint16_t keylen = 0;
    const uint8_t *key = cxex_signer_key(file, len, &keylen);
    if (!key) return CXEX_VERIFY_BAD_KEY;

    /* IDENTITY, most trusted first. The platform key is compared here rather
       than looked up in the vault because it is not in the vault - it is
       compiled into the kernel, so it cannot be added, removed or replaced by
       anything with write access to a disk. */
    if (same_bytes(key, keylen, cxos_trusted_key, cxos_trusted_key_len))
        return CX_TRUST_PLATFORM;

    if (in_vault(key, keylen))
        return CX_TRUST_PUBLISHER;

    /* Signed, intact, and by someone this machine has never been told to
       believe. That is a real and useful answer - it is not the same as
       tampered - and what to do about it is the caller's policy. */
    return CX_TRUST_UNVERIFIED;
}

const char *keyvault_trust_name(int level) {
    switch (level) {
        case CX_TRUST_PLATFORM:   return "platform";
        case CX_TRUST_PUBLISHER:  return "known publisher";
        case CX_TRUST_UNVERIFIED: return "unverified author";
        default:                  return "unsigned or invalid";
    }
}
