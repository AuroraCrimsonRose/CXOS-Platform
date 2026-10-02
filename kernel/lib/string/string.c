// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/string/string.c */
/* Aurora Tejeda */

#include "string.h"

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* case-insensitive compare (ASCII A-Z folded to a-z) */
int strcasecmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;            /* both ended together */
        a++; b++;
    }
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* WARNING: strcpy does NOT bound the write to dst's size - if src is longer
   than dst's buffer it overflows. Safe only when dst is provably large enough
   (e.g. a string literal of known length). For fixed buffers or any untrusted
   input, use strlcpy instead. */
char *strcpy(char *dst, const char *src) {
    char *p = dst;
    while ((*p++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

/* SAFE bounded copy: at most size-1 bytes, ALWAYS null-terminates (size>0).
   Returns strlen(src) so callers can detect truncation (ret >= size). */
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = 0;
    while (src[srclen]) srclen++;          /* length of src */
    if (size > 0) {
        size_t copy = (srclen < size - 1) ? srclen : (size - 1);
        for (size_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = '\0';                  /* always terminate */
    }
    return srclen;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        /* forward copy is safe when dst is below src */
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        /* dst overlaps above src: copy backward to avoid clobbering */
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}