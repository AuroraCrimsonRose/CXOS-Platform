/* /kernel/drivers/net/icmp.h */
/* Aurora Tejeda */
/*
 * ICMP - Stage 3. Implements echo request/reply, i.e. ping.
 *
 * icmp_ping sends an ICMP echo request to a target IP and waits (bounded) for
 * the matching echo reply, returning the round-trip tick count. This is the
 * "we're on the network" milestone.
 */

#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include "netif.h"

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

/* send one ICMP echo request to `dst` and wait for the reply.
   returns 1 on reply (and sets *rtt_ms to an approximate round-trip in ms),
   0 on timeout. seq is the echo sequence number to use. */
int icmp_ping(const ip4_t dst, uint16_t seq, uint32_t *rtt_us);

/* feed a received frame to ICMP: if it's an echo request for us, reply; if it's
   an echo reply we're waiting for, record it. returns 1 if consumed. */
int icmp_input(const uint8_t *frame, uint16_t len);

#endif