// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/crashrec.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The XCRL crash record: what a panic leaves in the CRASH partition for the
 * next boot to find (docs/kernel/CX_KERNEL_PLAN.md section 8, roadmap step 4).
 * No hardware here, so the format is tested on the host.
 *
 * ---- XCRL on disk (little-endian) -----------------------------------------
 * Sector 0 of the partition is the header; the text follows from sector 1.
 *
 *   0   4   magic    "XCRL"
 *   4   2   version  XCRL_VERSION
 *   6   2   kind     XCRL_KIND_*
 *   8   4   length   bytes of text, at most XCRL_TEXT_MAX
 *   12  4   crc      CRC-32 (IEEE) of bytes 0..11, then of the text
 *   16  496 zero
 *
 * The writer puts the text down first and the header last, so a panic that
 * dies mid-write leaves no header, or a header whose CRC the text fails. A
 * header that is all zero is an empty area. `cxk rig crashlog` and
 * `cxk verify-crashlog` read the same layout (devkit CXEX.Disk CrashRecord).
 */

#ifndef DIAG_CRASHREC_H
#define DIAG_CRASHREC_H

#include <stdint.h>

#define XCRL_MAGIC          0x4C524358u     /* "XCRL" */
#define XCRL_VERSION        1
#define XCRL_SECTOR         512u
#define XCRL_TEXT_MAX       (32u * XCRL_SECTOR)            /* 16 KB */
#define XCRL_SECTORS        (1u + XCRL_TEXT_MAX / XCRL_SECTOR)   /* header + text */

#define XCRL_KIND_CRASH     1u   /* a panic's log */
#define XCRL_KIND_REQUEST   2u   /* `cxk verify-crashlog` asks this boot to
                                    panic on purpose; carries no text */
#define XCRL_KIND_FAULT     3u   /* user processes that faulted after the
                                    self-tests, then the system log; a panic's
                                    record replaces it */
#define XCRL_KIND_BOOTLOG   4u   /* the whole system log as text, rewritten as
                                    the boot goes on. Never at sector 0: it has
                                    its own header at XCRL_BOOTLOG_LBA, so a
                                    crash or fault record never displaces it */

/* The boot log sits past the crash record (sectors 0..XCRL_SECTORS-1), header
   first and text after, in the same layout with a larger cap. `cxk rig
   bootlog` reads it, which is how a machine with no serial port gets its log
   to another one. */
#define XCRL_BOOTLOG_LBA    64u
#define XCRL_BOOTLOG_MAX    (256u * XCRL_SECTOR)           /* 128 KB */

/* The most text a record of this kind may carry. */
uint32_t xcrl_max_len(uint16_t kind);

/* What xcrl_check_header made of a header. */
enum xcrl_state {
    XCRL_VALID     =  0,
    XCRL_EMPTY     =  1,   /* no magic: nothing was ever written, or cleared */
    XCRL_BAD_VER   = -1,
    XCRL_BAD_KIND  = -2,
    XCRL_BAD_LEN   = -3,
    XCRL_BAD_CRC   = -4,
};

/* CRC-32 (IEEE 802.3, reflected, as zlib). Start with 0, chain by passing the
   last result back in. */
uint32_t xcrl_crc32(uint32_t crc, const void *data, uint32_t len);

/* Fill a 512-byte header for `len` bytes of `text`. A request carries none
   (text may be null when len is 0). */
void xcrl_seal(uint8_t hdr[XCRL_SECTOR], uint16_t kind, const char *text, uint32_t len);

/* Read a header's fields. XCRL_VALID means the magic, version, kind and length
   make sense; the CRC needs the text, so xcrl_check_text decides that. */
int xcrl_check_header(const uint8_t hdr[XCRL_SECTOR], uint16_t *kind, uint32_t *len);

/* With the text in hand: XCRL_VALID or XCRL_BAD_CRC. */
int xcrl_check_text(const uint8_t hdr[XCRL_SECTOR], const char *text, uint32_t len);

#endif
