// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/ksym.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The kernel's own symbol table, so a panic can name the functions in its
 * trace on a machine with nothing else to read it with.
 *
 * The table is generated from a first link of the kernel (`cxk ksyms`, in
 * tools/cmake/CMakeLists.txt) and linked into the second. It is read-only
 * data, which the linker script places after .text, so its size cannot move
 * a function: the addresses it records are the second link's too. A
 * self-test holds that (ktest_panic.c).
 */
#ifndef DIAG_KSYM_H
#define DIAG_KSYM_H

#include <stdint.h>

/* The generated table: ksym_offs ascending, from the start of the text. */
extern const uint32_t ksym_count;
extern const uint32_t ksym_offs[];
extern const uint32_t ksym_name_offs[];
extern const char     ksym_names[];

/* The name of the function covering `offset` bytes into the text, and how
   far in it is; 0 when no symbol is at or before it. */
const char *ksym_find(const uint32_t *offs, const uint32_t *name_offs, const char *names,
                      uint32_t count, uint32_t offset, uint32_t *off_out);

#ifndef CXK_HOST_TEST
/* ksym_find on the kernel's table. 0 for an address outside the text. */
const char *ksym_lookup(uintptr_t addr, uint32_t *off_out);
#endif

#endif
