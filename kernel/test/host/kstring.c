// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/kstring.c */
/* Aurora Tejeda / CATX Systems */
/*
 * The kernel's strlcpy, for kernel code built for the host.
 *
 * The host's C library has the rest of string.h, and a second memcpy would
 * fight it (and the sanitizers' interceptors); but it may have no strlcpy
 * (the Windows CRT) or one with other edge cases, and CXFS's handling of
 * untrusted names rests on this one's. So the kernel's own file is compiled
 * here with every other function renamed out of the way, and strlcpy is the
 * kernel's code, byte for byte.
 */
#define strlen     kstr_strlen
#define strcmp     kstr_strcmp
#define strcasecmp kstr_strcasecmp
#define strncmp    kstr_strncmp
#define strcpy     kstr_strcpy
#define strncpy    kstr_strncpy
#define memset     kstr_memset
#define memcpy     kstr_memcpy
#define memcmp     kstr_memcmp
#define memmove    kstr_memmove

#include "lib/string/string.c"
