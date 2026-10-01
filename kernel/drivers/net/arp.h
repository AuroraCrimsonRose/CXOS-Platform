/* /CXLite/kernel/drivers/arp.h */
/* Aurora Tejeda */
/*
 * Ethernet framing + ARP (Address Resolution Protocol).
 *
 * Ethernet: 14-byte header (dst MAC, src MAC, ethertype). ARP maps an IPv4
 * address to a MAC so we can address frames to hosts on the local network.
 * This is the first bidirectional milestone: send an ARP request, receive the
 * reply, cache the result.
 */

#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "netif.h"

#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IPV4  0x0800
#define ETH_HDR_LEN    14
#define ETH_ALEN       6

/* build an Ethernet header into buf (must be >=14 bytes). returns header len. */
int eth_build_header(uint8_t *buf, const uint8_t *dst_mac, uint16_t ethertype);

/* resolve an IPv4 address to a MAC: checks the cache, else sends an ARP request
   and polls (bounded) for the reply. on success copies the MAC into out_mac and
   returns 1; returns 0 on timeout/failure. */
int arp_resolve(const ip4_t ip, uint8_t *out_mac);

/* feed a received frame to ARP (handles replies + answers requests for our IP).
   returns 1 if it was an ARP frame we consumed, 0 otherwise. */
int arp_input(const uint8_t *frame, uint16_t len);

/* look up a cached entry without sending a request. 1 if found. */
int arp_cache_lookup(const ip4_t ip, uint8_t *out_mac);

#endif