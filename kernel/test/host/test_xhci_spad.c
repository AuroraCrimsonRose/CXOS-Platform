// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_xhci_spad.c */
/* Aurora Tejeda / CATX Systems */
/* How many scratchpad pages an xHCI controller wants (drivers/usb/xhci_spad.h):
   the count is split across HCSPARAMS2, high half in 25:21 and low in 31:27. */
#include "khost.h"
#include "drivers/usb/xhci_spad.h"

static uint32_t hcs2(uint32_t count) {   /* the register a controller asking for `count` reports */
    return ((count >> 5) << 21) | ((count & 0x1F) << 27) | 0x000000F1u;   /* other fields set */
}

KH_TEST(xhci_spad_count_from_both_halves) {
    uint32_t n = 99;
    KH_CHECK_EQ(xhci_spad_plan(0x000000F1u, 1, &n), 0); KH_CHECK_EQ(n, 0u);
    const uint32_t want[] = { 1, 4, 31, 32, 33, 63, 64, 512, 513, 1023 };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        KH_CHECK_EQ(xhci_spad_plan(hcs2(want[i]), 1, &n), 0);
        KH_CHECK_EQ(n, want[i]);
    }
}

KH_TEST(xhci_spad_needs_4k_pages) {
    uint32_t n = 99;
    KH_CHECK_EQ(xhci_spad_plan(hcs2(4), 0x3, &n), 0);    /* 4 KB among others */
    KH_CHECK_EQ(n, 4u);
    KH_CHECK_EQ(xhci_spad_plan(hcs2(4), 0x2, &n), -1);   /* 8 KB only */
    KH_CHECK_EQ(xhci_spad_plan(hcs2(4), 0x0, &n), -1);   /* reports none */
}
