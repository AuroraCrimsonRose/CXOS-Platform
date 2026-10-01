/* /CXLite/kernel/drivers/e1000.h */
/* Aurora Tejeda */
/*
 * Intel e1000 (82540EM, PCI 8086:100E) Ethernet driver - STAGE 1.
 *
 * Stage 1 brings up the NIC: find it on PCI, map its registers, read the MAC
 * address, set up the RX/TX descriptor rings (DMA), and send/receive raw
 * Ethernet frames. No protocol layers yet (Ethernet framing, ARP, IP, etc.
 * come in later stages on top of this).
 *
 * Target: QEMU's e1000 (the best-documented NIC for hobby OS dev). Link is
 * configured for 10 Mbps full-duplex per project goal; the emulated link
 * negotiates regardless, but we don't need high speed.
 */

#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* probe PCI for an e1000 NIC and initialize it (rings, MAC, link).
   returns 1 on success, 0 if no NIC found / init failed. */
int e1000_init(void);

int e1000_present(void);                 /* was a NIC found + initialized? */
const uint8_t *e1000_mac(void);          /* 6-byte MAC address */
int e1000_link_up(void);                 /* 1 if link is up */

/* send a raw Ethernet frame (len bytes). returns 0 on success, -1 on error. */
int e1000_send(const void *frame, uint16_t len);

/* poll for a received frame. if one is available, copies up to max_len bytes
   into buf and returns the frame length; returns 0 if nothing received. */
int e1000_receive(void *buf, uint16_t max_len);

/* diagnostics: frames transmitted OK, TX failures, frames received */
void e1000_stats(uint32_t *tx_ok, uint32_t *tx_fail, uint32_t *rx_ok);

#endif