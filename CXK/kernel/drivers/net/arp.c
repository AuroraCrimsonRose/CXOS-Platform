/* /CXLite/kernel/drivers/arp.c */
/* Aurora Tejeda */
/* Ethernet framing + ARP request/reply with a small cache. */

#include "arp.h"
#include "netif.h"
#include "sched.h"   /* yield() */

static const uint8_t bcast_mac[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* ---- small ARP cache ---- */
#define ARP_CACHE_SIZE 16
struct arp_entry { int valid; ip4_t ip; uint8_t mac[6]; };
static struct arp_entry cache[ARP_CACHE_SIZE];

static int ip_eq(const ip4_t a, const ip4_t b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void cache_put(const ip4_t ip, const uint8_t *mac) {
    /* update existing or take a free/oldest slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && ip_eq(cache[i].ip, ip)) {
            for (int k = 0; k < 6; k++) cache[i].mac[k] = mac[k];
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid) {
            cache[i].valid = 1;
            for (int k = 0; k < 4; k++) cache[i].ip[k] = ip[k];
            for (int k = 0; k < 6; k++) cache[i].mac[k] = mac[k];
            return;
        }
    }
    /* all full: overwrite slot 0 */
    cache[0].valid = 1;
    for (int k = 0; k < 4; k++) cache[0].ip[k] = ip[k];
    for (int k = 0; k < 6; k++) cache[0].mac[k] = mac[k];
}

int arp_cache_lookup(const ip4_t ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && ip_eq(cache[i].ip, ip)) {
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
    uint16_t oper = (uint16_t)(p[6] << 8) | p[7];
    const uint8_t *sha = p + 8;
    const uint8_t *spa = p + 14;
    const uint8_t *tpa = p + 24;

    /* learn the sender's IP->MAC mapping from any ARP we see */
    ip4_t sip; for (int i=0;i<4;i++) sip[i]=spa[i];
    cache_put(sip, sha);

    /* if it's a request for OUR ip, answer it */
    if (oper == 1) {
        const ip4_t *myip = &netif_cfg()->ip;
        int for_us = 1;
        for (int i=0;i<4;i++) if (tpa[i] != (*myip)[i]) { for_us = 0; break; }
        if (for_us) {
            ip4_t s; for (int i=0;i<4;i++) s[i]=spa[i];
            send_arp_reply(sha, s);
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