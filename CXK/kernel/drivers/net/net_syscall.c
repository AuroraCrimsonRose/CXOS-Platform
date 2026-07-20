/* /CXK/kernel/drivers/net/net_syscall.c */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * SYS_NET_OP handler: the bridge between ring-3 and the network stack.
 *
 * Like SYS_FB_OP this is one syscall with an `op` selector rather than a family
 * of syscalls - it keeps the syscall table small and matches the struct-pointer
 * ABI used everywhere else.
 *
 * IPv4 addresses cross the ABI as a packed u32 in network byte order, so
 * userspace (and X, which has no array-of-4 convention) needs no special type.
 */

#include "netif.h"
#include "icmp.h"
#include "../../cpu/usermode.h"    /* user_ptr_ok */
#include "../../../abi/cxk_abi.h"  /* net_op_args, NET_OP_*, E_* */

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

int sys_net_op(const struct net_op_args *ua) {
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct net_op_args a = *ua;           /* copy out of user space */

    switch (a.op) {

        case NET_OP_STATUS:
            return netif_ready() ? 1 : 0;

        case NET_OP_MAC: {
            if (a.len < 6) return E_RANGE;
            if (!user_ptr_ok((uint32_t)a.data, 6)) return E_FAULT;
            const uint8_t *m = netif_mac();
            uint8_t *dst = (uint8_t *)a.data;
            for (int i = 0; i < 6; i++) dst[i] = m[i];
            return 6;
        }

        case NET_OP_GET_IP: {
            if (!user_ptr_ok((uint32_t)a.out, 4 * sizeof(uint32_t))) return E_FAULT;
            const struct net_config *c = netif_cfg();
            if (!c) return E_NOENT;
            a.out[0] = pack_ip(c->ip);
            a.out[1] = pack_ip(c->mask);
            a.out[2] = pack_ip(c->gateway);
            a.out[3] = pack_ip(c->dns1);
            return 0;
        }

        case NET_OP_SET_IP: {
            ip4_t v;
            unpack_ip(a.ip, v);
            switch (a.len) {                       /* len selects the field */
                case 0: netif_set_ip(v);      break;
                case 1: netif_set_mask(v);    break;
                case 2: netif_set_gateway(v); break;
                case 3: {
                    const struct net_config *c = netif_cfg();
                    ip4_t d2 = { 0, 0, 0, 0 };
                    if (c) { d2[0]=c->dns2[0]; d2[1]=c->dns2[1]; d2[2]=c->dns2[2]; d2[3]=c->dns2[3]; }
                    netif_set_dns(v, d2);
                    break;
                }
                default: return E_INVAL;
            }
            return 0;
        }

        case NET_OP_PING: {
            if (!netif_ready()) return E_NOENT;
            ip4_t dst;
            unpack_ip(a.ip, dst);

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
                if (all_zero || all_ones || host_zero || host_ones) return E_INVAL;
            }
            uint32_t rtt = 0;
            /* icmp_ping returns 1 when a reply arrived, 0 on timeout. Pass that
               through unchanged - 1 = reply, 0 = no reply, negative = error.
               (An earlier comment here claimed 0 meant success, and the shell
               believed it, so every timeout was reported as a 0 ms reply.) */
            int r = icmp_ping(dst, (uint16_t)a.len, &rtt);   /* rtt in microseconds */
            if (a.out) {
                if (!user_ptr_ok((uint32_t)a.out, sizeof(uint32_t))) return E_FAULT;
                a.out[0] = r ? rtt : 0;            /* rtt is only valid on success */
            }
            return r;                              /* 1 = reply, 0 = timeout */
        }

        case NET_OP_SEND: {
            if (a.len == 0 || a.len > 1514) return E_RANGE;
            if (!user_ptr_ok((uint32_t)a.data, a.len)) return E_FAULT;
            return netif_send(a.data, (uint16_t)a.len);
        }

        case NET_OP_RECV: {
            if (a.len == 0) return E_RANGE;
            if (!user_ptr_ok((uint32_t)a.data, a.len)) return E_FAULT;
            return netif_receive(a.data, (uint16_t)a.len);
        }

        default:
            return E_INVAL;
    }
}
