/* /CXK/kernel/lib/format.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Pure number -> string formatting. No hardware, no state - it writes digits
 * into a caller-provided buffer, so it lives in lib/ and is testable in
 * isolation. The IDT panic dump, kmain's status lines, and the console all
 * need to turn numbers into text; this is the one shared implementation.
 *
 * (Pure, so by the lib convention it could be `fmt`; kept as `format` per the
 * project's k-prefix preference. The k here does NOT imply hardware access.)
 *
 * Each function writes a NUL-terminated string into `out` and returns the
 * length (excluding the NUL). `out` must be large enough:
 *   - u32 decimal: up to 10 digits + NUL  -> 11 bytes
 *   - u32 hex (8 digits) + NUL            -> 9 bytes  (no 0x prefix)
 *   - i32 decimal: up to 11 chars + NUL   -> 12 bytes
 */

#ifndef FORMAT_H
#define FORMAT_H

#include <stdint.h>
#include <stddef.h>

/* unsigned 32-bit -> decimal. Returns length written (excluding NUL). */
size_t fmt_u32(char *out, uint32_t v);

/* signed 32-bit -> decimal (leading '-' for negatives). */
size_t fmt_i32(char *out, int32_t v);

/* unsigned 32-bit -> hex. If `width` > 0, zero-pad to that many digits (max 8);
   if 0, emit the minimum digits. No "0x" prefix (caller adds it if wanted).
   `upper` selects A-F vs a-f. */
size_t fmt_hex(char *out, uint32_t v, int width, int upper);

#endif