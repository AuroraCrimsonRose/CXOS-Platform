// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/arp.c */
/* Aurora Tejeda */
/* Ethernet framing + ARP request/reply with a small cache. */

#include "arp.h"
#include "netif.h"
#include "sched.h"   /* yield() */
#include "timer.h"    /* timer_ticks / timer_since for cache aging */
#include "logging.h"

static const uint8_t bcast_mac[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* ---- small ARP cache ----
 *
 * The policy here answers the 2026-10-09 review's ARP finding
 * (GHSA-qgwr-mfqp-582m). It was: learn an IP->MAC mapping from ANY ARP frame
 * seen, taking the MAC from the frame's own sender-hardware field, and
 * overwrite any existing entry silently. A host on the same segment could
 * therefore point one of our mappings at itself.
 *
 * Of the systems worth comparing, only Haiku had actually solved this.
 * SerenityOS does what CXOS did, with a `FIXME: Protect against ARP spamming`
 * against it; NT5's TCP/IP validates rigorously and ages its cache but still
 * accepts the update on Ethernet (its don't-update-a-recently-good-entry rule
 * is scoped to token ring, for a protocol reason rather than a security one);
 * WRK has no ARP at all, and Redox's stack is a userspace daemon.
 *
 * So the rule is Haiku's - an entry that has been resolved is not replaced by
 * a DIFFERENT MAC, and the attempt is logged - with two pieces taken from NT5
 * that Haiku's design quietly depends on:
 *
 *   AGING.   Refuse-to-overwrite without expiry would trade poisoning for a
 *            permanently wrong entry: whoever answered first would own that IP
 *            until reboot. Entries carry a timestamp and expire, which is NT5's
 *            `ate_valid` + `ArpCacheLife`. A legitimate MAC change costs one
 *            cache lifetime instead of being impossible.
 *   STATIC.  NT5: "If the entry is already static, we'll want to leave it as
 *            static." The gateway comes from configuration, so it is never
 *            something to learn and never something to relearn.
 *
 * Solicitation tracking - only believing a reply to an outstanding request -
 * is the other common answer and is deliberately NOT used: frames here are
 * only processed during ping or ARP-resolution polling, so "was this
 * solicited?" is unanswerable much of the time. Haiku's rule needs no such
 * state, which is why it fits.
 */
#define ARP_CACHE_SIZE 16

/* One minute. Long enough that a resolve is not repeated per packet, short
   enough that a wrong or stale mapping heals without a reboot. */
#define ARP_ENTRY_TTL_MS 60000u

struct arp_entry {
    int      valid;
    int      is_static;   /* configured, not learned: never replaced or aged */
    ip4_t    ip;
    uint8_t  mac[6];
    uint32_t learned_ms;  /* timer_ticks() when this mapping was accepted */
};
static struct arp_entry cache[ARP_CACHE_SIZE];

static int ip_eq(const ip4_t a, const ip4_t b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int mac_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Wrap-safe, via timer_since - a 32-bit tick counter wraps every 49.7 days and
   a plain subtraction against a stored value is the bug Phase 2.5 fixed in the
   timer driver. */
static int entry_expired(const struct arp_entry *e) {
    if (e->is_static) return 0;
    return timer_since(e->learned_ms) >= ARP_ENTRY_TTL_MS;
}

static void entry_set(struct arp_entry *e, const ip4_t ip, const uint8_t *mac,
                      int is_static) {
    e->valid = 1;
    e->is_static = is_static;
    for (int k = 0; k < 4; k++) e->ip[k] = ip[k];
    for (int k = 0; k < 6; k++) e->mac[k] = mac[k];
    e->learned_ms = timer_ticks();
}

/* Accept a mapping. Returns 1 if the cache now holds it, 0 if the update was
   REFUSED - which is a normal outcome and not an error. */
static int cache_put(const ip4_t ip, const uint8_t *mac, int is_static) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid || !ip_eq(cache[i].ip, ip)) continue;

        /* Configuration outranks the network, always. */
        if (cache[i].is_static && !is_static) return 0;

        /* Same MAC: just a refresh, and refreshing is the point of seeing
           traffic from a host we already know. */
        if (mac_eq(cache[i].mac, mac)) {
            cache[i].learned_ms = timer_ticks();
            return 1;
        }

        /* A DIFFERENT MAC for an address we have already resolved. If the
           entry is still live this is refused and said out loud, because the
           benign causes (a replaced NIC, a failover) are rare and the
           malicious one is cheap. Once the entry has aged out, relearning is
           exactly what should happen. */
        if (!entry_expired(&cache[i])) {
            klog("ARP", SEV_WARN, "refused: mapping already resolved to a different MAC");
            klog_child_u32("  ip .", (uint32_t)ip[3], LOG_COLOR_VALUE, "");
            return 0;
        }

        entry_set(&cache[i], ip, mac, is_static);
        return 1;
    }

    /* Not cached. Take a free slot, preferring one that has expired. */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid) { entry_set(&cache[i], ip, mac, is_static); return 1; }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (entry_expired(&cache[i])) { entry_set(&cache[i], ip, mac, is_static); return 1; }
    }

    /* Full, and nothing has expired. Evicting a live entry to make room for an
       unverifiable one is how a flood of invented addresses would clear the
       cache, so the new mapping is dropped instead. A static entry could never
       be evicted anyway. */
    return 0;
}

/* Install a mapping from configuration. Outranks anything learned. */
void arp_cache_set_static(const ip4_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && ip_eq(cache[i].ip, ip)) {
            entry_set(&cache[i], ip, mac, 1);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid || entry_expired(&cache[i])) {
            entry_set(&cache[i], ip, mac, 1);
            return;
        }
    }
    entry_set(&cache[0], ip, mac, 1);
}

int arp_cache_lookup(const ip4_t ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && ip_eq(cache[i].ip, ip)) {
            /* An expired entry is not an answer: returning it would make the
               TTL decorative, since nothing else ever clears a slot. */
            if (entry_expired(&cache[i])) { cache[i].valid = 0; return 0; }
            for (int k = 0; k < 6; k++) out_mac[k] = cache[i].mac[k];
            return 1;
        }
    }
    return 0;
}

int eth_build_header(uint8_t *buf, const uint8_t *dst_mac, uint16_t ethertype) {
    const uint8_t *src = netif_mac();
    for (int i = 0; i < 6; i++) buf[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) buf[6 + i] = src[i];
    buf[12] = (uint8_t)(ethertype >> 8);
    buf[13] = (uint8_t)(ethertype & 0xFF);
    return ETH_HDR_LEN;
}

/* ARP packet layout (Ethernet/IPv4): 28 bytes after the Ethernet header.
   htype(2) ptype(2) hlen(1) plen(1) oper(2) sha(6) spa(4) tha(6) tpa(4) */
static void build_arp(uint8_t *p, uint16_t oper,
                      const uint8_t *tha, const ip4_t tpa) {
    const uint8_t *sha = netif_mac();
    const ip4_t *spa = &netif_cfg()->ip;
    p[0]=0x00; p[1]=0x01;            /* htype = Ethernet */
    p[2]=0x08; p[3]=0x00;            /* ptype = IPv4 */
    p[4]=6;    p[5]=4;               /* hlen, plen */
    p[6]=(uint8_t)(oper>>8); p[7]=(uint8_t)(oper&0xFF);
    for (int i=0;i<6;i++) p[8+i]=sha[i];
    for (int i=0;i<4;i++) p[14+i]=(*spa)[i];
    for (int i=0;i<6;i++) p[18+i]=tha[i];
    for (int i=0;i<4;i++) p[24+i]=tpa[i];
}

static void send_arp_request(const ip4_t target) {
    uint8_t frame[ETH_HDR_LEN + 28];
    eth_build_header(frame, bcast_mac, ETH_TYPE_ARP);
    uint8_t zero[6] = {0,0,0,0,0,0};
    build_arp(frame + ETH_HDR_LEN, 1 /*request*/, zero, target);
    netif_send(frame, sizeof(frame));
}

static void send_arp_reply(const uint8_t *to_mac, const ip4_t to_ip) {
    uint8_t frame[ETH_HDR_LEN + 28];
    eth_build_header(frame, to_mac, ETH_TYPE_ARP);
    build_arp(frame + ETH_HDR_LEN, 2 /*reply*/, to_mac, to_ip);
    netif_send(frame, sizeof(frame));
}

int arp_input(const uint8_t *frame, uint16_t len) {
    if (len < ETH_HDR_LEN + 28) return 0;
    uint16_t etype = (uint16_t)(frame[12] << 8) | frame[13];
    if (etype != ETH_TYPE_ARP) return 0;

    const uint8_t *p = frame + ETH_HDR_LEN;

    /* Say what kind of ARP this is before believing any address in it. None of
       this was checked, so a frame declaring a different hardware or protocol
       type - or different address lengths - still populated the IPv4 cache
       with whatever happened to sit at those offsets. NT5's TCP/IP rejects on
       each of these separately (`ARP_HW_ENET`, the address-length test, the
       protocol-type test) and Haiku does the same; it is the cheapest part of
       this fix and the one with no policy question attached. */
    uint16_t htype = (uint16_t)(p[0] << 8) | p[1];
    uint16_t ptype = (uint16_t)(p[2] << 8) | p[3];
    uint8_t  hlen  = p[4];
    uint8_t  plen  = p[5];
    uint16_t oper  = (uint16_t)(p[6] << 8) | p[7];

    if (htype != ARP_HW_ETHERNET)   return 0;   /* not Ethernet hardware */
    if (ptype != ETH_TYPE_IPV4)     return 0;   /* not IPv4 addresses */
    if (hlen != 6 || plen != 4)     return 0;   /* not the lengths those imply */
    if (oper != ARP_OP_REQUEST && oper != ARP_OP_REPLY) return 0;

    const uint8_t *spa = p + 14;
    const uint8_t *tpa = p + 24;

    /* The sender's MAC comes from the ETHERNET header, not from the ARP
       payload's sender-hardware field. The payload field is whatever the
       sender chose to write; the Ethernet source is what actually carried the
       frame. Haiku does the same - it passes `buffer->source` to
       arp_update_entry rather than `header.hardware_sender`. The two agree for
       every honest frame, so this costs nothing and removes one thing an
       attacker gets to pick. */
    const uint8_t *src_mac = frame + 6;

    /* Learn from any ARP we see, request or reply. That is Haiku's behaviour
       too ("remember the address of the sender as we might need it later"),
       and it is safe here because cache_put refuses to replace a live mapping
       with a different MAC - the refusal is what carries the safety, not a
       restriction on which frames teach. */
    ip4_t sip; for (int i=0;i<4;i++) sip[i]=spa[i];
    (void)cache_put(sip, src_mac, 0);

    /* if it's a request for OUR ip, answer it */
    if (oper == ARP_OP_REQUEST) {
        const ip4_t *myip = &netif_cfg()->ip;
        int for_us = 1;
        for (int i=0;i<4;i++) if (tpa[i] != (*myip)[i]) { for_us = 0; break; }
        if (for_us) {
            ip4_t s; for (int i=0;i<4;i++) s[i]=spa[i];
            /* Reply to where the request actually came from, not to the MAC
               the payload claims to be from - the same reasoning as the cache
               update above. A request forged with someone else's sender
               field would otherwise have our reply delivered to them. */
            send_arp_reply(src_mac, s);
        }
    }
    return 1;
}

int arp_resolve(const ip4_t ip, uint8_t *out_mac) {
    if (arp_cache_lookup(ip, out_mac)) return 1;

    /* send a request, then poll for frames feeding ARP until the cache fills
       or we time out. */
    for (int attempt = 0; attempt < 3; attempt++) {
        send_arp_request(ip);
        for (int i = 0; i < 200000; i++) {
            uint8_t buf[1600];
            int n = netif_receive(buf, sizeof(buf));
            if (n > 0) arp_input(buf, (uint16_t)n);
            if (arp_cache_lookup(ip, out_mac)) return 1;
            if ((i & 0xFF) == 0) yield();   /* keep the machine responsive */
        }
    }
    return 0;
}