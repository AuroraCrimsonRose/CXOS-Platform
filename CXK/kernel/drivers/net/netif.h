/* /CXLite/kernel/drivers/netif.h */
/* Aurora Tejeda */
/*
 * Network interface abstraction + configuration.
 *
 * This sits between the protocol stack (Ethernet/ARP/IP/...) and the actual NIC
 * driver (e1000 today; NE2000 / RTL8168 could drop in later behind the same
 * send/receive interface - the stack above never changes).
 *
 * Also holds the static network configuration (IP, mask, gateway, DNS) which is
 * set from the shell. Per design: values are taken as-is from the user with no
 * validation - it's the user's responsibility to enter correct numbers.
 */

#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>

/* IPv4 address as 4 bytes, network order (a.b.c.d -> [a][b][c][d]) */
typedef uint8_t ip4_t[4];

/* the active network configuration (static for now; DHCP later) */
struct net_config {
    ip4_t ip;        /* this host's IP */
    ip4_t mask;      /* subnet mask */
    ip4_t gateway;   /* default gateway */
    ip4_t dns1;      /* primary DNS */
    ip4_t dns2;      /* secondary DNS */
};

/* bring up the interface layer (defaults + bind to the NIC). returns 1 if a NIC
   is present. call after the NIC driver is initialized. */
int netif_init(void);

int netif_ready(void);                 /* NIC present + interface up? */
const uint8_t *netif_mac(void);        /* our 6-byte MAC */
const struct net_config *netif_cfg(void);

/* set configuration fields (from the shell). values stored as-is, no validation. */
void netif_set_ip(const ip4_t ip);
void netif_set_mask(const ip4_t mask);
void netif_set_gateway(const ip4_t gw);
void netif_set_dns(const ip4_t dns1, const ip4_t dns2);

/* send/receive raw Ethernet frames via the bound NIC (NIC-independent) */
int netif_send(const void *frame, uint16_t len);
int netif_receive(void *buf, uint16_t max_len);

/* parse "a.b.c.d" into out[4]. returns 1 on success (4 dotted parts), 0 if the
   format is unparseable. (Per design we don't validate ranges - 999 is fine -
   but we still need 4 dot-separated numeric fields to store anything.) */
int netif_parse_ip(const char *s, ip4_t out);

/* format ip into "a.b.c.d" in buf (>=16 bytes) */
void netif_ip_str(const ip4_t ip, char *buf);

#endif