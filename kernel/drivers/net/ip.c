// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/net/ip.c */
/* Aurora Tejeda */
/* IPv4 layer: header build/parse, checksum, routing + ARP next-hop. */

#include "ip.h"
#include "arp.h"
#include "netif.h"

static uint16_t ip_id_counter = 0;

uint16_t ip_checksum(const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1) sum += (uint32_t)(p[0] << 8);   /* odd byte, high */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* decide the next-hop IP: if dst is on our subnet, it's dst itself; otherwise
   it's the gateway. (mask compare: (a & mask) == (b & mask)) */
static void next_hop(const ip4_t dst, ip4_t hop) {
    const struct net_config *c = netif_cfg();
    int on_subnet = 1;
    for (int i = 0; i < 4; i++) {
        if ((dst[i] & c->mask[i]) != (c->ip[i] & c->mask[i])) { on_subnet = 0; break; }
    }
    const uint8_t *src = on_subnet ? dst : c->gateway;
    for (int i = 0; i < 4; i++) hop[i] = src[i];
}

int ip_send(const ip4_t dst, uint8_t protocol, const void *payload, uint16_t len) {
    if (!netif_ready()) return -1;

    /* resolve the next hop's MAC via ARP */
    ip4_t hop;
    next_hop(dst, hop);
    uint8_t dst_mac[6];
    if (!arp_resolve(hop, dst_mac)) return -1;

    /* assemble Ethernet + IP + payload in one buffer */
    uint8_t frame[ETH_HDR_LEN + IP_HDR_LEN + 1500];
    if (len > 1500) return -1;

    eth_build_header(frame, dst_mac, ETH_TYPE_IPV4);
    uint8_t *ip = frame + ETH_HDR_LEN;
    const struct net_config *c = netif_cfg();

    uint16_t total = IP_HDR_LEN + len;
    ip[0] = 0x45;                         /* version 4, IHL 5 (20 bytes) */
    ip[1] = 0x00;                         /* DSCP/ECN */
    ip[2] = (uint8_t)(total >> 8);        /* total length */
    ip[3] = (uint8_t)(total & 0xFF);
    uint16_t id = ip_id_counter++;
    ip[4] = (uint8_t)(id >> 8);           /* identification */
    ip[5] = (uint8_t)(id & 0xFF);
    ip[6] = 0x00;                         /* flags / frag offset */
    ip[7] = 0x00;
    ip[8] = 64;                           /* TTL */
    ip[9] = protocol;
    ip[10] = 0x00;                        /* checksum (filled below) */
    ip[11] = 0x00;
    for (int i = 0; i < 4; i++) ip[12 + i] = c->ip[i];   /* source */
    for (int i = 0; i < 4; i++) ip[16 + i] = dst[i];     /* dest */

    uint16_t csum = ip_checksum(ip, IP_HDR_LEN);
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)(csum & 0xFF);

    /* copy payload */
    const uint8_t *pl = (const uint8_t *)payload;
    for (uint16_t i = 0; i < len; i++) ip[IP_HDR_LEN + i] = pl[i];

    return netif_send(frame, (uint16_t)(ETH_HDR_LEN + total));
}

int ip_parse(const uint8_t *frame, uint16_t frame_len,
             ip4_t src_ip, uint8_t *protocol,
             const uint8_t **payload, uint16_t *payload_len) {
    if (frame_len < ETH_HDR_LEN + IP_HDR_LEN) return 0;
    uint16_t etype = (uint16_t)(frame[12] << 8) | frame[13];
    if (etype != ETH_TYPE_IPV4) return 0;

    const uint8_t *ip = frame + ETH_HDR_LEN;
    uint16_t avail = (uint16_t)(frame_len - ETH_HDR_LEN);   /* IP bytes received */

    uint8_t version = (ip[0] >> 4) & 0x0F;
    uint8_t ihl = (ip[0] & 0x0F) * 4;     /* header length in bytes */
    if (version != 4 || ihl < IP_HDR_LEN) return 0;

    /* The declared header must be ENTIRELY PRESENT before any field past the
       first 20 bytes is trusted, and before `ihl` is used to place the payload
       pointer. IHL may be up to 15 (60 bytes) and the frame-length check above
       only guarantees 20, so a 34-byte frame declaring IHL 15 used to produce
       `*payload = ip + 60` - forty bytes past the end of the frame.

       This test is REDUNDANT with the two below, and knowing that is worth
       more than removing it: `total >= ihl` and `total <= avail` together
       imply `ihl <= avail`, so no frame can reach the checks below and fail
       here. Confirmed by sabotage - deleting this line turns no test red,
       because every case it would catch is caught by `total > avail`.

       It stays for one reason: `ip_checksum(ip, ihl)` below reads `ihl` bytes,
       and this makes that read's safety a fact established three lines above
       it rather than a property derived from two later comparisons. Deriving
       it is exactly the reasoning that produced the original bug. */
    if (ihl > avail) return 0;

    uint16_t total = (uint16_t)(ip[2] << 8) | ip[3];
    if (total < ihl) return 0;

    /* A packet that declares more than arrived is MALFORMED, and is refused
       rather than clamped. Clamping was the bug: `total` was reduced to the
       received length without re-testing it against `ihl`, so `total - ihl`
       underflowed and `*payload_len` came back as much as 65496 on a frame of
       34 bytes. The caller then had a length that was not merely wrong but
       enormous, and `icmp_input` echoes that payload back to the sender - so
       the kernel disclosed whatever lay past the frame in its receive buffer.
       Truncation is not something a receiver can repair; the only safe answer
       is to drop the packet. */
    if (total > avail) return 0;

    /* Fragments are refused, not reassembled. The flags/fragment-offset field
       was written as zero on send and never read on receive, so a fragment
       arrived looking exactly like a complete datagram and its partial payload
       was handed to the protocol handler as a whole one. Reassembly is real
       work and is not done; until it is, saying no is the honest behaviour.
       MF = bit 5 of ip[6]; the 13-bit offset is the low 5 bits of ip[6] plus
       all of ip[7]. DF (bit 6) is not a fragment and is ignored. */
    uint16_t frag = (uint16_t)(((ip[6] & 0x1F) << 8) | ip[7]);
    if ((ip[6] & 0x20) || frag != 0) return 0;

    /* The header checksum, which was computed on send and never verified on
       receive - so a corrupted packet was indistinguishable from a good one.
       Checked over the declared header length, which is why it comes after
       `ihl` has been proved present. A correct header sums to zero. */
    if (ip_checksum(ip, (int)ihl) != 0) return 0;

    for (int i = 0; i < 4; i++) src_ip[i] = ip[12 + i];
    *protocol = ip[9];
    *payload = ip + ihl;
    *payload_len = (uint16_t)(total - ihl);
    return 1;
}