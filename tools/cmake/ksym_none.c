// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /tools/cmake/ksym_none.c */
/* An empty symbol table, for the first of the kernel's two links: that link
   exists to give `cxk ksyms` the addresses (kernel/diag/ksym.h). */
#include <stdint.h>

const uint32_t ksym_count = 0;
const uint32_t ksym_offs[1] = { 0 };
const uint32_t ksym_name_offs[1] = { 0 };
const char     ksym_names[1] = { 0 };
