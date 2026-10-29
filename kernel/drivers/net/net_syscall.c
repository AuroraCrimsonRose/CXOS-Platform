// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/net/net_syscall.c */
/* Aurora Tejeda / CATX Systems */
/*
 * The SYS_NET_* handlers: the bridge between ring-3 and the network stack.
 * One syscall per operation, its arguments in registers; the GRANT_NET gate
 * is in front of them, in cpu/usermode.c.
 *
 * IPv4 addresses cross the ABI as a packed u32 in network byte order, so
 * userspace (and X, which has no array-of-4 convention) needs no special type.
 */

#include "netif.h"
#include "icmp.h"
#include "../../cpu/usermode.h"    /* user_ptr_readable / user_ptr_writable */
#include "../../../abi/cxk_abi.h"  /* net_ip_config, E_* */

/* packed u32 (network order) -> ip4_t */
static void unpack_ip(uint32_t v, ip4_t out) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

/* ip4_t -> packed u32 (network order) */
static uint32_t pack_ip(const ip4_t a) {
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
           ((uint32_t)a[2] << 8)  | (uint32_t)a[3];
}

int sys_net_status(void) {
    return netif_ready() ? 1 : 0;
}

int sys_net_mac(uint64_t buf, uint64_t len) {
    if (len < 6) return E_BUFFER_TOO_SMALL;
    if (!user_ptr_writable(buf, 6)) return E_BAD_ADDRESS;
    const uint8_t *m = netif_mac();
    uint8_t *dst = (uint8_t *)user_ptr(buf);
    for (int i = 0; i < 6; i++) dst[i] = m[i];
    return 6;
}

int sys_net_get_config(uint64_t uc) {
    if (!user_ptr_writable(uc, sizeof(struct net_ip_config))) return E_BAD_ADDRESS;
    const struct net_config *c = netif_cfg();
    if (!c) return E_NOT_FOUND;
    struct net_ip_config *out = (struct net_ip_config *)user_ptr(uc);
    out->ip      = pack_ip(c->ip);
    out->mask    = pack_ip(c->mask);
    out->gateway = pack_ip(c->gateway);
    out->dns     = pack_ip(c->dns1);
    return 0;
}

int sys_net_set_ip(uint32_t addr) {
    ip4_t v;
    unpack_ip(addr, v);
    netif_set_ip(v);
    return 0;
}

int sys_net_set_mask(uint32_t addr) {
    ip4_t v;
    unpack_ip(addr, v);
    netif_set_mask(v);
    return 0;
}

int sys_net_set_gateway(uint32_t addr) {
    ip4_t v;
    unpack_ip(addr, v);
    netif_set_gateway(v);
    return 0;
}

int sys_net_set_dns(uint32_t addr) {
    ip4_t v;
    unpack_ip(addr, v);
    const struct net_config *c = netif_cfg();
    ip4_t d2 = { 0, 0, 0, 0 };
    if (c) { d2[0]=c->dns2[0]; d2[1]=c->dns2[1]; d2[2]=c->dns2[2]; d2[3]=c->dns2[3]; }
    netif_set_dns(v, d2);
    return 0;
}

int sys_net_ping(uint32_t addr, uint32_t seq, uint64_t urtt) {
    if (!netif_ready()) return E_NOT_FOUND;
    ip4_t dst;
    unpack_ip(addr, dst);

    /* Reject addresses that can never answer an echo, rather than
       burning a full one-second timeout each on four of them (which
       looked like a lockup). Covers 0.0.0.0, the all-ones broadcast,
       and - for the configured mask - the network and broadcast
       addresses of the local subnet. */
    const struct net_config *nc = netif_cfg();
    if (nc) {
        int all_zero = 1, all_ones = 1, host_zero = 1, host_ones = 1;
        for (int i = 0; i < 4; i++) {
            if (dst[i] != 0)    all_zero = 0;
            if (dst[i] != 255)  all_ones = 0;
            uint8_t host = (uint8_t)(dst[i] & (uint8_t)~nc->mask[i]);
            if (host != 0)                          host_zero = 0;
            if (host != (uint8_t)~nc->mask[i])      host_ones = 0;
        }
        if (all_zero || all_ones || host_zero || host_ones) return E_INVALID_ARGS;
    }
    uint32_t rtt = 0;
    /* icmp_ping returns 1 when a reply arrived, 0 on timeout. Pass that
       through unchanged - 1 = reply, 0 = no reply, negative = error.
       (An earlier comment here claimed 0 meant success, and the shell
       believed it, so every timeout was reported as a 0 ms reply.) */
    int r = icmp_ping(dst, (uint16_t)seq, &rtt);   /* rtt in microseconds */
    if (urtt) {
        if (!user_ptr_writable(urtt, sizeof(uint32_t))) return E_BAD_ADDRESS;
        *(uint32_t *)user_ptr(urtt) = r ? rtt : 0;     /* rtt is only valid on success */
    }
    return r;                                      /* 1 = reply, 0 = timeout */
}

int sys_net_send(uint64_t frame, uint64_t len) {
    if (len == 0 || len > 1514) return E_OUT_OF_RANGE;
    if (!user_ptr_readable(frame, (uint32_t)len)) return E_BAD_ADDRESS;
    return netif_send(user_ptr(frame), (uint16_t)len);
}

int sys_net_recv(uint64_t buf, uint64_t cap) {
    if (cap == 0) return E_BUFFER_TOO_SMALL;
    if (cap > 0xFFFF) cap = 0xFFFF;   /* more than any frame; the driver takes a u16 */
    if (!user_ptr_writable(buf, (uint32_t)cap)) return E_BAD_ADDRESS;
    return netif_receive(user_ptr(buf), (uint16_t)cap);
}
