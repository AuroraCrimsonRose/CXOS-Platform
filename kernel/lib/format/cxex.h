/* /kernel/lib/cxex.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXEX executable format definitions (CX_EXTENSION_SYSTEM.md section 9).
 *
 * This is the single source of truth in kernel code for what a CXEX file
 * (.xkex / .xbex / .xuex) looks like. It provides the structs callers use plus
 * pure parse/validate/accessor helpers. It owns the on-disk byte layout via
 * EXPLICIT-OFFSET parsing (not packed structs / casts), so it is immune to
 * compiler padding/alignment surprises and matches the bytes mkcxes.py writes
 * exactly.
 *
 * Scope: format knowledge ONLY - definitions, parsing, validation, accessors.
 * It does NOT place sections in memory, read from disk, or run anything. A
 * loader is a separate module that CALLS this. (Hashing/verification likewise
 * live in sha256/rsa; this just locates the regions for them.)
 */

#ifndef CXEX_H
#define CXEX_H

#include <stdint.h>
#include <stddef.h>

/* ---- magic ---- */
#define CXEX_MAGIC0 'C'
#define CXEX_MAGIC1 'X'
#define CXEX_MAGIC2 'E'
#define CXEX_MAGIC3 'X'

/* ---- type codes (mirror the extension family) ---- */
/* Each code is the extension's domain letter followed by 'E' for executive,
   and the domain says WHOSE the program is - which is what caps_for() reads to
   decide the authority it starts with.

   0x4345 'CE' (.xuex) is deliberately absent. It named an executable for how
   it was built, and every executable here is compiled, so it distinguished
   nothing - a kernel image is as compiled as an application. Programs are
   named by owner instead. See docs/formats/CX_EXTENSION_SYSTEM.md. */
#define CXEX_TYPE_KERNEL 0x4B45u   /* 'KE' the kernel          (.xkex) */
#define CXEX_TYPE_BOOT   0x4245u   /* 'BE' the boot chain      (.xbex) */
#define CXEX_TYPE_OS     0x4F45u   /* 'OE' the OS proper       (.xoex) */
#define CXEX_TYPE_SYSTEM 0x5345u   /* 'SE' OS-owned, not the OS(.xsex) */
#define CXEX_TYPE_USER   0x5545u   /* 'UE' a user's            (.xuex) */

/* ---- arch ---- */
#define CXEX_ARCH_X86_32 1u

/* ---- header flags (section 9.6) ---- */
#define CXEX_FLAG_EXECUTABLE         (1u << 0)
#define CXEX_FLAG_RELOCATABLE        (1u << 1)
#define CXEX_FLAG_SIGNED             (1u << 2)
#define CXEX_FLAG_KERNEL_PRIV        (1u << 3)
#define CXEX_FLAG_REQUIRE_ABI_MATCH  (1u << 4)
#define CXEX_FLAG_REQUIRE_ARCH_MATCH (1u << 5)

/* ---- section flags (9.4) ---- */
#define CXEX_SEC_READ   (1u << 0)
#define CXEX_SEC_WRITE  (1u << 1)
#define CXEX_SEC_EXEC   (1u << 2)
#define CXEX_SEC_NOBITS (1u << 3)

/* ---- sizes of the on-disk structures (explicit, not sizeof a struct) ---- */
#define CXEX_HEADER_SIZE   56      /* section 9.3 */
#define CXEX_SECTION_SIZE  28      /* section 9.4: 8+4+4+4+4+4 */
/* signature block (10.3): magic(4) sig_algo(2) hash_algo(2) fingerprint(32)
   sig_len(2) signature(sig_len). Fixed part before the signature = 42 bytes. */
/* CXSG block header, up to the two variable-length parts that follow it:
     0  "CXSG"        4
     4  sig_algo      2
     6  hash_algo     2
     8  fingerprint  32   sha256 of the .xkpk bytes below
    40  pubkey_len    2
    42  sig_len       2
    44  pubkey     pubkey_len   the signer's .xkpk file, verbatim
        signature  sig_len                                              */
#define CXEX_SIG_HDR_SIZE  44

#define CXSG_MAGIC0 'C'
#define CXSG_MAGIC1 'X'
#define CXSG_MAGIC2 'S'
#define CXSG_MAGIC3 'G'

/* ---- structs callers work with (in-memory, not the on-disk image) ---- */

struct cxex_header {
    uint16_t type_code;
    uint16_t format_version;
    uint16_t arch_target;
    uint16_t abi_version;
    uint32_t flags;
    uint32_t entry_point;
    uint32_t load_base;
    uint32_t image_min;
    uint32_t image_max;
    uint16_t section_count;
    uint16_t section_offset;
    uint32_t reloc_offset;
    uint32_t signature_offset;
    uint32_t dependency_offset;
    uint32_t phys_base;         /* physical load base (ELF LMA). boot/cxexload.asm
                                   reads this at offset 48 to compute the
                                   virt->phys delta for the higher-half kernel.
                                   NOT reserved - do not zero it. */
};

struct cxex_section {
    char     name[9];          /* 8 chars + NUL */
    uint32_t file_offset;
    uint32_t virt_addr;
    uint32_t file_size;
    uint32_t mem_size;
    uint32_t flags;
};

struct cxex_sig {
    uint16_t sig_algo;
    uint16_t hash_algo;
    uint8_t  fingerprint[32];   /* sha256 of the .xkpk at pubkey_file_offset */
    uint16_t pubkey_len;
    uint16_t sig_len;
    uint32_t pubkey_file_offset; /* the signer's .xkpk, carried with the image */
    uint32_t sig_file_offset;    /* where the signature bytes start in the file */
};

/* ---- parse / validate / accessors (own the byte layout) ---- */

/* Validate magic and read the header. Returns 0 on success, negative on error:
   -1 too small, -2 bad magic. Does NOT enforce arch/abi (see cxex_check_compat). */
int cxex_parse_header(const uint8_t *file, size_t len, struct cxex_header *out);

/* Check arch/abi compatibility against this kernel, honoring the REQUIRE_* flags.
   Returns 0 if loadable here, negative on mismatch. */
int cxex_check_compat(const struct cxex_header *h,
                      uint16_t my_arch, uint16_t my_abi);

/* Read section table entry `i` (0-based). Returns 0 on success, negative on
   error (-1 index out of range, -2 entry runs past EOF). */
int cxex_get_section(const uint8_t *file, size_t len,
                     const struct cxex_header *h, uint16_t i,
                     struct cxex_section *out);

/* Is this image signed (SIGNED flag + a signature_offset present)? */
int cxex_is_signed(const struct cxex_header *h);

/* Read the CXSG signature block header (if signed). Returns 0 on success,
   negative on error (-1 not signed, -2 bad offset/size, -3 bad CXSG magic). */
int cxex_get_sig(const uint8_t *file, size_t len,
                 const struct cxex_header *h, struct cxex_sig *out);

#endif