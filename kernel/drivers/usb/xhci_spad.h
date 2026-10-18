// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/xhci_spad.h */
/* Aurora Tejeda / CATX Systems */
/*
 * xHCI scratchpad buffers (xHCI 1.2 section 4.20): pages of memory a
 * controller asks the driver for and uses for its own state. Entry 0 of the
 * DCBAA points at an array of their physical addresses. A controller that
 * asks for some and is given none never works, and QEMU's asks for none, so
 * no boot under QEMU reaches this: the plan is host-tested and the builder has
 * a boot self-test of its own.
 */

#ifndef XHCI_SPAD_H
#define XHCI_SPAD_H

#include <stdint.h>
#include "arch/paging.h"   /* paddr_t */

#define XHCI_SPAD_MAX  1023u   /* the 10 bits HCSPARAMS2 has for the count */

/* How many scratchpad pages the controller wants, from HCSPARAMS2 (high five
   bits of the count in 25:21, low five in 31:27) and the PAGESIZE register,
   in which bit n means pages of 2^(n+12) bytes are supported. Returns 0 with
   *count set, or -1 if the controller cannot use 4 KB pages: every structure
   this driver gives it is a 4 KB page. */
static inline int xhci_spad_plan(uint32_t hcsparams2, uint32_t pagesize, uint32_t *count) {
    if (!(pagesize & 1u)) return -1;
    *count = (((hcsparams2 >> 21) & 0x1Fu) << 5) | ((hcsparams2 >> 27) & 0x1Fu);
    return 0;
}

/* Allocate `count` zeroed pages and the array that lists them (64-byte
   aligned; contiguous, two pages past 512 entries). Returns 0 with *array set,
   0 when count is 0, or -1 with everything freed. */
int xhci_spad_build(uint32_t count, paddr_t *array);

/* Entry i of an array xhci_spad_build made. */
uint64_t xhci_spad_entry(paddr_t array, uint32_t i);

/* Free what xhci_spad_build allocated, given the same count. */
void xhci_spad_release(paddr_t array, uint32_t count);

#endif
