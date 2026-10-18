// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/boot/cxbi.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The loader's handover, struct cxk_boot_info (abi/cxk_boot.h): the only
 * thing the kernel takes from the loader (docs/boot/CX_BOOT.md section 3.3,
 * roadmap step 4). The memory map, the framebuffer, the ACPI RSDP and the
 * CPUID the loader saw all come from here, never from an address the loader
 * happened to leave something at.
 */

#ifndef BOOT_CXBI_H
#define BOOT_CXBI_H

#include <stdint.h>
#include "cxk_boot.h"

/* Take the handover at physical `phys`, which the loader passed (in EBX on
   x86). Copies the structure and its memory map into the kernel, so nothing
   reads the loader's pages again. A missing or unknown handover is fatal:
   without a memory map there is nothing to run in. First thing in kmain after
   the console. */
void cxbi_init(uintptr_t phys);

/* The handover, as copied. */
const struct cxk_boot_info *cxbi(void);

/* The memory map: entries 0..cxbi_mmap_count()-1. A map longer than the
   kernel keeps is cut short, and cxbi_mmap_dropped() says by how many. */
uint32_t cxbi_mmap_count(void);
const struct cxk_mmap_entry *cxbi_mmap(uint32_t i);
uint32_t cxbi_mmap_dropped(void);

#endif
