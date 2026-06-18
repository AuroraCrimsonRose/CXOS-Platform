/* /CXK/kernel/drivers/net/ip.h */
/* Aurora Tejeda */
/*
 * IPv4 layer - Stage 3.
 *
 * Builds and parses IPv4 packets on top of the Ethernet/ARP layer. Handles the
 * header checksum, the local-vs-gateway routing decision (is the destination on
 * our subnet, or do we send via the gateway?), and ARP-resolving the next hop.
 *
 * Upper protocols (ICMP now; UDP later) build their payload and hand it here
 * with a protocol number; ip_send wraps it in IP + Ethernet and transmits.
 */

#ifndef IP_H
#define IP_H

#include <stdint.h>
#include "netif.h"

#define IP_PROTO_ICMP  1
#define IP_PROTO_UDP   17

#define IP_HDR_LEN     20

/* compute the standard 16-bit one's-complement checksum over `len` bytes. */
uint16_t ip_checksum(const void *data, int len);

/* send an IPv4 packet: wrap `payload` (len bytes) with an IP header for the
   given protocol and destination, resolve the next hop via ARP, frame it in
   Ethernet, and transmit. returns 0 on success, -1 on error (no route / ARP
   failed / send failed). */
int ip_send(const ip4_t dst, uint8_t protocol, const void *payload, uint16_t len);

/* parse a received Ethernet frame as IPv4. on success, fills the out-params
   with pointers/values describing the packet and returns 1; returns 0 if the
   frame isn't IPv4 or is malformed.
     src_ip   : source address (4 bytes, copied out)
     protocol : IP protocol number
     payload  : set to point at the L4 payload within `frame`
     payload_len : length of that payload */
int ip_parse(const uint8_t *frame, uint16_t frame_len,
             ip4_t src_ip, uint8_t *protocol,
             const uint8_t **payload, uint16_t *payload_len);

#endif