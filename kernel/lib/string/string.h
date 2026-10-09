// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/string/string.h */
/* Aurora Tejeda */
/* Freestanding string / memory helpers (no libc available). */

#ifndef STRING_H
#define STRING_H

#include <stdint.h>
#include <stddef.h>

/* length of a null-terminated string */
size_t strlen(const char *s);

/* ====================================================================
 * The bounded-string contract (engineering §2)
 *
 * One convention for every kernel function that writes into a buffer the
 * caller supplies with a capacity - the string helpers below, and equally
 * the formatters that are not in this file (disk_capacity_str, cxfs_path_of,
 * fmt_u32 and the rest). The review's point was that each subsystem had
 * invented its own edge-case behaviour; these are the rules they all follow,
 * and a new one is wrong if it does something else.
 *
 *   1. `cap == 0` means write nothing and do not dereference the buffer.
 *      Not even a terminator - there is nowhere to put one. A negative `cap`
 *      is treated as zero, because an `int` capacity can carry one.
 *   2. With `cap > 0` the result is ALWAYS null-terminated, including when
 *      it had to be cut short and when the operation failed.
 *   3. Truncation is reported, never silent. A function returning a length
 *      returns the length the source NEEDED (strlcpy's BSD semantics), so
 *      `ret >= cap` is the test for "it did not fit". A function returning a
 *      status returns an error. A `void` function must not truncate at all:
 *      if it cannot say so, it must not do it.
 *   4. Truncation is the caller's decision, not the callee's. A caller that
 *      copies untrusted input into a fixed buffer checks for it and refuses,
 *      unless truncation is explicitly what it wants - the shape Haiku uses
 *      for exactly this, where ddm_strlcpy() turns a truncating copy into
 *      B_NAME_TOO_LONG unless the caller passes allowTruncation
 *      (src/system/kernel/disk_device_manager/ddm_userland_interface.cpp).
 *      This matters because a check applied to an already-truncated copy is
 *      not a check: cxfs_create_entry and cxfs_rename both truncated a name
 *      into a 64-byte buffer and then asked cxfs_normalize_name whether it
 *      was too long, which it never could be by then.
 *   5. A NULL destination is permitted only with `cap == 0`; a NULL source is
 *      always an error. Haiku's user_strlcpy draws the line in the same
 *      place (B_BAD_VALUE vs B_BAD_ADDRESS, vm.cpp), and the asymmetry is
 *      deliberate: (NULL, 0) is the useful "how long would this be?" call,
 *      while there is no useful read from nowhere.
 *
 * Partial output is not a success. A formatter that cannot fit the whole
 * result emits nothing meaningful rather than a fragment that reads as
 * complete - disk_capacity_str used to print the unit with no number when the
 * buffer was small, so a 500 GB disk read as "GB".
 * ==================================================================== */

/* compare two strings: 0 if equal, <0 or >0 otherwise */
int strcmp(const char *a, const char *b);

/* case-insensitive compare (ASCII): 0 if equal ignoring letter case */
int strcasecmp(const char *a, const char *b);

/* compare at most n characters */
int strncmp(const char *a, const char *b, size_t n);

/* copy src (incl. null terminator) into dst; returns dst */
char *strcpy(char *dst, const char *src);

/* copy at most n bytes */
char *strncpy(char *dst, const char *src, size_t n);

/* SAFE bounded copy (OpenBSD-style). Copies at most size-1 bytes from src into
   dst and ALWAYS null-terminates (as long as size > 0). Returns the length of
   src, so truncation is detectable: truncation happened if the return value is
   >= size. Prefer this over strcpy/strncpy when copying into a fixed buffer,
   especially for untrusted input. */
size_t strlcpy(char *dst, const char *src, size_t size);

/* memory fill / copy / compare */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memmove(void *dst, const void *src, size_t n);

#endif