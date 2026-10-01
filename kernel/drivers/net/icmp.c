/* /kernel/drivers/net/icmp.c */
/* Aurora Tejeda */
/* ICMP echo request/reply (ping), on top of the IP layer. */

#include "icmp.h"
#include "ip.h"
#include "arp.h"
#include "netif.h"
#include "timer.h"
#include "sched.h"   /* yield() */

/* state for an in-flight ping (single outstanding request at a time) */
static volatile int   waiting = 0;
static uint16_t       wait_id = 0;
static uint16_t       wait_seq = 0;
static volatile int   got_reply = 0;
static uint16_t       our_ident = 0x4358;   /* "CX" */

/* build an ICMP echo packet (type 8 request / type 0 reply) into buf.
   layout: type(1) code(1) checksum(2) id(2) seq(2) data(...).  returns length */
static uint16_t build_echo(uint8_t *buf, uint8_t type, uint16_t id,
                           uint16_t seq, const uint8_t *data, uint16_t dlen) {
    buf[0] = type;
    buf[1] = 0;            /* code */
    buf[2] = 0; buf[3] = 0; /* checksum (filled after) */
    buf[4] = (uint8_t)(id >> 8);  buf[5] = (uint8_t)(id & 0xFF);
    buf[6] = (uint8_t)(seq >> 8); buf[7] = (uint8_t)(seq & 0xFF);
    for (uint16_t i = 0; i < dlen; i++) buf[8 + i] = data[i];
    uint16_t total = (uint16_t)(8 + dlen);
    uint16_t csum = ip_checksum(buf, total);
    buf[2] = (uint8_t)(csum >> 8);
    buf[3] = (uint8_t)(csum & 0xFF);
    return total;
}

int icmp_input(const uint8_t *frame, uint16_t len) {
    ip4_t src; uint8_t proto;
    const uint8_t *pl; uint16_t pl_len;
    if (!ip_parse(frame, len, src, &proto, &pl, &pl_len)) return 0;
    if (proto != IP_PROTO_ICMP) return 0;
    if (pl_len < 8) return 1;   /* malformed but it was ICMP */

    uint8_t type = pl[0];
    uint16_t id  = (uint16_t)(pl[4] << 8) | pl[5];
    uint16_t seq = (uint16_t)(pl[6] << 8) | pl[7];

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        /* someone is pinging us - reply by echoing the payload back */
        uint16_t dlen = (uint16_t)(pl_len - 8);
        if (dlen > 64) dlen = 64;
        uint8_t reply[8 + 64];
        uint16_t rlen = build_echo(reply, ICMP_TYPE_ECHO_REPLY, id, seq,
                                   pl + 8, dlen);
        ip_send(src, IP_PROTO_ICMP, reply, rlen);
        return 1;
    }

    if (type == ICMP_TYPE_ECHO_REPLY) {
        if (waiting && id == wait_id && seq == wait_seq) {
            got_reply = 1;
            waiting = 0;
        }
        return 1;
    }
    return 1;
}

int icmp_ping(const ip4_t dst, uint16_t seq, uint32_t *rtt_us) {
    if (!netif_ready()) return 0;

    /* 32 bytes of simple payload */
    uint8_t data[32];
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)('a' + (i % 26));

    uint8_t pkt[8 + 32];
    uint16_t plen = build_echo(pkt, ICMP_TYPE_ECHO_REQUEST, our_ident, seq, data, 32);

    waiting = 1; got_reply = 0; wait_id = our_ident; wait_seq = seq;

    uint32_t start_tick = timer_ticks();
    uint32_t start_tsc  = timer_tsc32();      /* microsecond-resolution start */
    if (ip_send(dst, IP_PROTO_ICMP, pkt, plen) < 0) { waiting = 0; return 0; }

    /* Poll for the reply, bounded (~1 s at 1000 Hz). yield() on each pass so a
       timeout does not lock the machine: this runs inside a syscall on the
       caller's thread, and without yielding nothing else gets scheduled for the
       whole second (four of those looked like a freeze). */
    while ((timer_ticks() - start_tick) < 1000) {
        uint8_t buf[1600];
        int n = netif_receive(buf, sizeof(buf));
        if (n > 0) {
            /* hand to ARP first (so gateway ARP still resolves), then ICMP */
            arp_input(buf, (uint16_t)n);
            icmp_input(buf, (uint16_t)n);
        }
        if (got_reply) {
            /* microseconds, not milliseconds: a SLIRP reply arrives in far less
               than one PIT tick, which is why this always read 0 ms before */
            if (rtt_us) *rtt_us = timer_us_since(start_tsc);
            return 1;
        }
        yield();
    }
    waiting = 0;
    return 0;
}