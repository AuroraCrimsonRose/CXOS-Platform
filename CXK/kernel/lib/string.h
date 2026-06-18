/* /CXLite/kernel/lib/string.h */
/* Aurora Tejeda */
/* Freestanding string / memory helpers (no libc available). */

#ifndef STRING_H
#define STRING_H

#include <stdint.h>
#include <stddef.h>

/* length of a null-terminated string */
size_t strlen(const char *s);

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