// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_net.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Malformed-packet cases for the IPv4 and ARP parsers.
 *
 * The shape is ktest_loader.c's, for the same reasons it works there: build one
 * known-good frame, make every case a SINGLE mutation of it, and keep the
 * known-good frame as a case of its own - so the suite cannot pass by refusing
 * everything, which is the way a validator test fails silently.
 *
 * What these would have caught, had they existed:
 *
 *   - ip_parse used `ihl` to place the payload pointer without ever checking it
 *     against the bytes received. IHL may declare 60 bytes of header; the frame
 *     length check only guarantees 20. A 34-byte frame declaring IHL 15 came
 *     back with a payload pointer 40 bytes past the end of the frame.
 *   - ip_parse clamped a declared length larger than the frame down to the
 *     frame, without re-testing it against `ihl`. `total - ihl` then underflowed
 *     and the reported payload length reached 65496 - and icmp_input echoes
 *     that payload back to the sender, so the kernel disclosed whatever lay
 *     past the frame in its receive buffer.
 *
 * Both are now refusals, and both assert on the OUTPUTS rather than only the
 * return value: ip_parse returned 1 in the broken case, so a test that only
 * checked "did it accept?" would have passed on the bug.
 */

#include <stdint.h>
#include "ktest_net.h"
#include "ip.h"
#include "arp.h"
#include "netif.h"
#include "logging.h"

#define ETH_HDR 14
#define IPOFF   ETH_HDR          /* where the IP header starts in the frame */

/* ---- one known-good IPv4/ICMP frame, and the knobs to bend it ---- */

struct ipframe {
    uint8_t  buf[128];
    uint16_t len;                /* what we tell the parser arrived */
};

/* Fills `f` with a well-formed IPv4 frame carrying `payload_len` bytes of ICMP,
   a correct checksum, and no fragmentation. */
static void ip_good(struct ipframe *f, uint16_t payload_len) {
    for (unsigned i = 0; i < sizeof f->buf; i++) f->buf[i] = 0;

    uint8_t *e = f->buf;
    /* dst/src MAC left zero; only the EtherType is parsed */
    e[12] = 0x08; e[13] = 0x00;                  /* ETH_TYPE_IPV4 */

    uint8_t *ip = f->buf + IPOFF;
    uint16_t total = (uint16_t)(IP_HDR_LEN + payload_len);
    ip[0] = 0x45;                                /* version 4, IHL 5 (20 bytes) */
    ip[1] = 0x00;                                /* DSCP/ECN */
    ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)(total & 0xFF);
    ip[4] = 0x12; ip[5] = 0x34;                  /* id */
    ip[6] = 0x00; ip[7] = 0x00;                  /* flags + fragment offset: none */
    ip[8] = 64;                                  /* TTL */
    ip[9] = IP_PROTO_ICMP;
    ip[10] = 0x00; ip[11] = 0x00;                /* checksum, filled below */
    ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = 2;      /* source */
    ip[16] = 10; ip[17] = 0; ip[18] = 2; ip[19] = 15;     /* dest */

    /* some recognisable payload, so a wrong pointer is visible */
    for (uint16_t i = 0; i < payload_len; i++) ip[IP_HDR_LEN + i] = (uint8_t)(0xA0 + (i & 0x0F));

    uint16_t c = ip_checksum(ip, IP_HDR_LEN);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)(c & 0xFF);

    f->len = (uint16_t)(ETH_HDR + total);
}

/* Recompute the header checksum after a mutation, for the cases that are
   testing something other than the checksum. */
static void ip_refix_csum(struct ipframe *f) {
    uint8_t *ip = f->buf + IPOFF;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
    if (ihl < IP_HDR_LEN) ihl = IP_HDR_LEN;
    ip[10] = 0; ip[11] = 0;
    uint16_t c = ip_checksum(ip, (int)ihl);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)(c & 0xFF);
}

static void ip_set_total(struct ipframe *f, uint16_t total) {
    uint8_t *ip = f->buf + IPOFF;
    ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)(total & 0xFF);
}

static void ip_set_ihl(struct ipframe *f, uint8_t words) {
    uint8_t *ip = f->buf + IPOFF;
    ip[0] = (uint8_t)(0x40 | (words & 0x0F));
}

/* Runs ip_parse and reports whether it accepted. On acceptance the outputs are
   handed back so a case can assert on them - the point of the whole exercise,
   since the underflow bug returned 1 with a poisoned length. */
static int ip_try(const struct ipframe *f, const uint8_t **pl, uint16_t *pl_len) {
    ip4_t src; uint8_t proto;
    const uint8_t *p = 0; uint16_t n = 0xFFFF;
    int ok = ip_parse(f->buf, f->len, src, &proto, &p, &n);
    if (pl) *pl = p;
    if (pl_len) *pl_len = n;
    return ok;
}

static int refuses(const struct ipframe *f) { return ip_try(f, 0, 0) == 0; }

int ktest_ip_parse_adversarial(void) {
    struct ipframe f;
    const uint8_t *pl; uint16_t pl_len;
    int ok = 1;

    /* ---- the control: a good frame is accepted, with the RIGHT outputs ----
       Without this the rest proves nothing: a parser that refuses everything
       passes every refusal case. */
    ip_good(&f, 16);
    if (!ip_try(&f, &pl, &pl_len)) return 0;
    ok = ok && (pl == f.buf + IPOFF + IP_HDR_LEN);
    ok = ok && (pl_len == 16);
    ok = ok && (pl[0] == 0xA0);                    /* the payload we planted */
    if (!ok) return 0;

    /* A zero-length payload is legal: total == ihl exactly. */
    ip_good(&f, 0);
    if (!ip_try(&f, &pl, &pl_len)) return 0;
    ok = ok && (pl_len == 0);

    /* ---- bug 1: IHL beyond the bytes received ----
       Every IHL from 6 to 15 on a frame holding only a 20-byte header. Each
       declares a header longer than arrived, so each must be refused - and
       before the fix each produced a payload pointer up to 40 bytes past the
       end of the frame. */
    for (uint8_t words = 6; words <= 15; words++) {
        ip_good(&f, 0);                 /* frame is exactly ETH + 20 */
        ip_set_ihl(&f, words);
        ip_set_total(&f, (uint16_t)(words * 4));   /* total >= ihl, so that check passes */
        ip_refix_csum(&f);
        ok = ok && refuses(&f);
    }

    /* An IHL larger than 5 IS legal when the options are actually present. */
    ip_good(&f, 40);                    /* 40 spare bytes after the base header */
    ip_set_ihl(&f, 10);                 /* 40-byte header: 20 base + 20 options */
    ip_set_total(&f, 60);
    ip_refix_csum(&f);
    if (!ip_try(&f, &pl, &pl_len)) return 0;
    ok = ok && (pl == f.buf + IPOFF + 40);
    ok = ok && (pl_len == 20);

    /* ---- bug 2: a declared length larger than the frame ----
       This is the underflow. `total` used to be clamped to the received length
       with no re-test against `ihl`, so these produced a payload length of
       65496 and a return value of 1. */
    ip_good(&f, 0);                     /* 34-byte frame */
    ip_set_ihl(&f, 15);                 /* 60-byte header declared */
    ip_set_total(&f, 60);
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    /* the same shape without the IHL mutation: total overruns the frame */
    ip_good(&f, 8);
    ip_set_total(&f, 1400);             /* declares far more than arrived */
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    /* one byte more than arrived is still a refusal - the boundary, not just
       the obvious case */
    ip_good(&f, 8);
    ip_set_total(&f, (uint16_t)(IP_HDR_LEN + 8 + 1));
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    /* exactly as much as arrived is accepted: the boundary from the other side */
    ip_good(&f, 8);
    if (!ip_try(&f, &pl, &pl_len)) return 0;
    ok = ok && (pl_len == 8);

    /* ---- total smaller than the header ---- */
    ip_good(&f, 16);
    ip_set_total(&f, 19);               /* below IP_HDR_LEN */
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    /* ---- IHL below the minimum ---- */
    for (uint8_t words = 0; words <= 4; words++) {
        ip_good(&f, 16);
        ip_set_ihl(&f, words);
        ip_refix_csum(&f);
        ok = ok && refuses(&f);
    }

    /* ---- wrong version ---- */
    ip_good(&f, 16);
    f.buf[IPOFF] = (uint8_t)(0x60 | 5);           /* version 6 */
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    /* ---- not IPv4 at the Ethernet layer ---- */
    ip_good(&f, 16);
    f.buf[12] = 0x08; f.buf[13] = 0x06;           /* ARP */
    ok = ok && refuses(&f);

    /* ---- truncated below a minimum frame ---- */
    ip_good(&f, 16);
    f.len = ETH_HDR + IP_HDR_LEN - 1;
    ok = ok && refuses(&f);
    f.len = 0;
    ok = ok && refuses(&f);

    /* ---- fragments ----
       MF set, and a non-zero offset, each on their own. Both used to be
       accepted and handed to the protocol handler as whole datagrams. */
    ip_good(&f, 16);
    f.buf[IPOFF + 6] = 0x20;                      /* MF */
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    ip_good(&f, 16);
    f.buf[IPOFF + 6] = 0x00; f.buf[IPOFF + 7] = 0x01;   /* offset 1 */
    ip_refix_csum(&f);
    ok = ok && refuses(&f);

    /* DF is not a fragment and must still be accepted, so the check above is
       testing the right bit. */
    ip_good(&f, 16);
    f.buf[IPOFF + 6] = 0x40;                      /* DF */
    ip_refix_csum(&f);
    ok = ok && (ip_try(&f, 0, 0) == 1);

    /* ---- the header checksum ----
       Never verified before, so a corrupted header was indistinguishable from
       a good one. */
    ip_good(&f, 16);
    f.buf[IPOFF + 10] ^= 0xFF;                    /* corrupt the checksum itself */
    ok = ok && refuses(&f);

    ip_good(&f, 16);
    f.buf[IPOFF + 8] = 1;                         /* change the TTL, leave the sum */
    ok = ok && refuses(&f);

    ip_good(&f, 16);
    f.buf[IPOFF + 12] ^= 0x01;                    /* change the source address */
    ok = ok && refuses(&f);

    return ok;
}

/* ---- ARP ----
 * arp_input is bounds-safe and was before this: it checks the frame length and
 * every read is inside the 28 bytes that check guarantees. What it does not do
 * is validate the fields that say what KIND of addresses it is about to
 * believe, so an ARP frame declaring a different hardware or protocol type, or
 * different address lengths, still populated the IPv4 cache.
 *
 * The cache-update POLICY - that any ARP frame, solicited or not, teaches the
 * cache - is a separate question and deliberately not asserted here. It is a
 * decision to be made (Phase 2.5) rather than a bug to fix, and a test written
 * before the decision would just pin today's behaviour.
 */

static int mac_is(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void arp_good(uint8_t *buf, uint16_t *len) {
    for (int i = 0; i < 64; i++) buf[i] = 0;
    buf[12] = 0x08; buf[13] = 0x06;               /* ETH_TYPE_ARP */

    uint8_t *p = buf + ETH_HDR;
    p[0] = 0x00; p[1] = 0x01;                     /* hardware type: Ethernet */
    p[2] = 0x08; p[3] = 0x00;                     /* protocol type: IPv4 */
    p[4] = 6;                                     /* hlen */
    p[5] = 4;                                     /* plen */
    p[6] = 0x00; p[7] = 0x02;                     /* operation: reply */
    for (int i = 0; i < 6; i++) p[8 + i] = (uint8_t)(0x10 + i);    /* sender MAC */
    p[14] = 10; p[15] = 0; p[16] = 2; p[17] = 99;                  /* sender IP */
    for (int i = 0; i < 6; i++) p[18 + i] = 0xFF;                  /* target MAC */
    p[24] = 10; p[25] = 0; p[26] = 2; p[27] = 15;                  /* target IP */

    *len = ETH_HDR + 28;
}

int ktest_arp_input_adversarial(void) {
    uint8_t buf[64]; uint16_t len;
    int ok = 1;

    /* the control */
    arp_good(buf, &len);
    if (arp_input(buf, len) != 1) return 0;

    /* short frames are refused, at the boundary and below it */
    arp_good(buf, &len);
    ok = ok && (arp_input(buf, (uint16_t)(ETH_HDR + 28 - 1)) == 0);
    ok = ok && (arp_input(buf, ETH_HDR) == 0);
    ok = ok && (arp_input(buf, 0) == 0);

    /* not ARP at the Ethernet layer */
    arp_good(buf, &len);
    buf[12] = 0x08; buf[13] = 0x00;               /* IPv4 */
    ok = ok && (arp_input(buf, len) == 0);

    /* ---- the fields that say what kind of ARP this is (2026-10-09 §2) ----
       None of these were checked, so a frame declaring different hardware or
       protocol types - or different address lengths - still populated the
       IPv4 cache from whatever sat at those offsets. */
    arp_good(buf, &len); buf[ETH_HDR + 1] = 6;    /* hardware type: not Ethernet */
    ok = ok && (arp_input(buf, len) == 0);
    arp_good(buf, &len); buf[ETH_HDR + 2] = 0x86; buf[ETH_HDR + 3] = 0xDD;  /* IPv6 */
    ok = ok && (arp_input(buf, len) == 0);
    arp_good(buf, &len); buf[ETH_HDR + 4] = 8;    /* hlen != 6 */
    ok = ok && (arp_input(buf, len) == 0);
    arp_good(buf, &len); buf[ETH_HDR + 5] = 16;   /* plen != 4 */
    ok = ok && (arp_input(buf, len) == 0);
    arp_good(buf, &len); buf[ETH_HDR + 7] = 9;    /* opcode neither request nor reply */
    ok = ok && (arp_input(buf, len) == 0);

    /* ---- the cache refuses to be re-pointed (the poisoning case) ----
       An address resolved once is not replaced by a different MAC while the
       entry is live. This is the finding itself: before it, the second frame
       below silently took the mapping.
       A fresh IP is used so this does not depend on what earlier tests or the
       real network left in the cache. */
    ip4_t victim = { 10, 0, 2, 231 };
    uint8_t first[6]  = { 0xAA, 0, 0, 0, 0, 0x01 };
    uint8_t second[6] = { 0xBB, 0, 0, 0, 0, 0x02 };
    uint8_t got[6];

    arp_good(buf, &len);
    for (int i = 0; i < 6; i++) buf[6 + i] = first[i];           /* Ethernet source */
    buf[ETH_HDR + 14] = 10; buf[ETH_HDR + 15] = 0;
    buf[ETH_HDR + 16] = 2;  buf[ETH_HDR + 17] = 231;             /* sender IP */
    ok = ok && (arp_input(buf, len) == 1);
    ok = ok && arp_cache_lookup(victim, got) && mac_is(got, first);

    /* the same host again is a refresh, not a conflict */
    ok = ok && (arp_input(buf, len) == 1);
    ok = ok && arp_cache_lookup(victim, got) && mac_is(got, first);

    /* a different MAC for that IP must NOT take it */
    for (int i = 0; i < 6; i++) buf[6 + i] = second[i];
    arp_input(buf, len);
    ok = ok && arp_cache_lookup(victim, got) && mac_is(got, first);

    /* ---- the MAC is taken from the Ethernet source, not the payload ----
       The payload's sender-hardware field is whatever the sender chose to
       write. Here the two disagree, and the Ethernet source must win. */
    ip4_t other = { 10, 0, 2, 232 };
    uint8_t ethsrc[6]  = { 0xCC, 0, 0, 0, 0, 0x03 };
    uint8_t claimed[6] = { 0xDD, 0, 0, 0, 0, 0x04 };
    arp_good(buf, &len);
    for (int i = 0; i < 6; i++) buf[6 + i] = ethsrc[i];          /* Ethernet source */
    for (int i = 0; i < 6; i++) buf[ETH_HDR + 8 + i] = claimed[i];  /* payload sha */
    buf[ETH_HDR + 14] = 10; buf[ETH_HDR + 15] = 0;
    buf[ETH_HDR + 16] = 2;  buf[ETH_HDR + 17] = 232;
    ok = ok && (arp_input(buf, len) == 1);
    ok = ok && arp_cache_lookup(other, got);
    ok = ok && mac_is(got, ethsrc) && !mac_is(got, claimed);

    /* ---- a static entry outranks the network ---- */
    ip4_t gw = { 10, 0, 2, 233 };
    uint8_t cfg[6] = { 0xEE, 0, 0, 0, 0, 0x05 };
    arp_cache_set_static(gw, cfg);
    arp_good(buf, &len);
    for (int i = 0; i < 6; i++) buf[6 + i] = second[i];
    buf[ETH_HDR + 14] = 10; buf[ETH_HDR + 15] = 0;
    buf[ETH_HDR + 16] = 2;  buf[ETH_HDR + 17] = 233;
    arp_input(buf, len);
    ok = ok && arp_cache_lookup(gw, got) && mac_is(got, cfg);

    return ok;
}
