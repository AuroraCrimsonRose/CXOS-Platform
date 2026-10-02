/* /kernel/drivers/net/rtl8169.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Realtek RTL8111 / RTL8168 / RTL8169 Gigabit Ethernet driver.
 *
 * One driver, three names, because they are the same silicon lineage:
 *
 *   RTL8111  the part number printed on a motherboard's integrated NIC. This
 *            is what a 990FX / 970 + SB950 board almost always carries, and it
 *            is why this driver exists - the e1000 is an Intel part and will
 *            never match anything on such a board.
 *   RTL8168  the PCI device ID that RTL8111 actually reports (10EC:8168).
 *            The marketing name and the ID do not agree; the ID is what we
 *            match on.
 *   RTL8169  the original PCI part, and the name of the descriptor model all
 *            of them use ("C+ mode"). Linux calls the whole family r8169.
 *
 * Structured like e1000.c on purpose: probe PCI, map registers, read the MAC,
 * set up RX/TX descriptor rings in a fixed DMA window, and poll. No interrupts
 * - the stack above polls, and adding IRQs here would be a second thing to get
 * wrong while this driver is still unverified on real silicon.
 *
 * NOT TESTED ON HARDWARE. QEMU has no RTL8169/8168 model (only the unrelated
 * RTL8139), so this has been verified to compile, to probe cleanly, and to
 * leave a machine without the chip exactly as it found it - and nothing more.
 * Every register write below is traceable to the datasheet ordering; the init
 * path logs its progress precisely so that the first boot on a real board
 * produces a diagnosis rather than silence.
 */

#ifndef RTL8169_H
#define RTL8169_H

#include <stdint.h>

/* probe PCI for an RTL8111/8168/8169 and initialize it (rings, MAC, link).
   returns 1 on success, 0 if no NIC found / init failed. */
int rtl8169_init(void);

int rtl8169_present(void);                 /* was a NIC found + initialized? */
const uint8_t *rtl8169_mac(void);          /* 6-byte MAC address */
int rtl8169_link_up(void);                 /* 1 if link is up */

/* send a raw Ethernet frame (len bytes). returns 0 on success, -1 on error. */
int rtl8169_send(const void *frame, uint16_t len);

/* poll for a received frame. if one is available, copies up to max_len bytes
   into buf and returns the frame length; returns 0 if nothing received. */
int rtl8169_receive(void *buf, uint16_t max_len);

/* diagnostics: frames transmitted OK, TX failures, frames received */
void rtl8169_stats(uint32_t *tx_ok, uint32_t *tx_fail, uint32_t *rx_ok);

#endif
