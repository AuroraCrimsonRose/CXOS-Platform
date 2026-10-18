// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/iowin.h */
/* Aurora Tejeda / CATX Systems */
/*
 * I/O windows: how a driver reaches a device's registers
 * (docs/kernel/CX_KERNEL_PLAN.md section 2.2).
 *
 * A window is a range of device registers, either I/O ports or memory-mapped,
 * read and written by offset from its start. Only the window knows which kind
 * it is, so a driver's logic is the same for both: a 16550 or an ATA channel
 * is port-mapped on an old PC and memory-mapped behind many PCI cards. The
 * port backend is the architecture's (arch/pio.h); nothing else in generic
 * code executes an I/O instruction. SerenityOS does the same
 * (Kernel/Library/IOWindow.cpp), as does NetBSD's bus_space.
 *
 * A window refuses an access that does not lie wholly inside it: a read gives
 * all ones, as from a bus with nothing on it, and a write is dropped. Either
 * is a driver bug, logged once per window, and counted in iowin_refusals.
 */

#ifndef DRIVERS_IOWIN_H
#define DRIVERS_IOWIN_H

#include <stdint.h>

enum iowin_kind { IOWIN_NONE = 0, IOWIN_PORT = 1, IOWIN_MMIO = 2 };

struct iowin {
    uintptr_t base;     /* first port, or the mapped virtual address */
    uint32_t  size;     /* bytes (ports) in the window */
    uint8_t   kind;     /* enum iowin_kind */
    uint8_t   warned;   /* a refusal has been logged */
};

/* Open a window over `size` ports from `port`. Returns 0, or -1 (and leaves the
   window closed) if the range is empty or runs past the last port, or the
   architecture has no I/O ports. */
int iowin_port(struct iowin *w, uint32_t port, uint32_t size);

/* A port window fixed at build time, for a static initializer: the legacy PC
   devices, whose ports never move. A range past port 0xFFFF does not compile. */
#define IOWIN_PORTS(port, size)                                               \
    { (uintptr_t)(port),                                                      \
      (size) + 0 * sizeof(char[(port) + (size) <= 0x10000u && (size) ? 1 : -1]), \
      IOWIN_PORT, 0 }

/* Open a window over `size` bytes of device memory the caller has already
   mapped (uncached) at `base`. Returns 0, or -1 if the range is empty or wraps. */
int iowin_mmio(struct iowin *w, volatile void *base, uint32_t size);

uint8_t  iowin_read8 (struct iowin *w, uint32_t off);
uint16_t iowin_read16(struct iowin *w, uint32_t off);
uint32_t iowin_read32(struct iowin *w, uint32_t off);
void     iowin_write8 (struct iowin *w, uint32_t off, uint8_t  v);
void     iowin_write16(struct iowin *w, uint32_t off, uint16_t v);
void     iowin_write32(struct iowin *w, uint32_t off, uint32_t v);

/* Accesses refused since boot, by every window. */
extern uint32_t iowin_refusals;

#endif
