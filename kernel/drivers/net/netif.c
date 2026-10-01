/* /CXLite/kernel/drivers/netif.c */
/* Aurora Tejeda */
/* Network interface abstraction + static config.
 *
 * Binds to whichever NIC is actually on the bus. Two exist: the Intel e1000,
 * which is what QEMU emulates, and the Realtek RTL8111/8168, which is what a
 * real desktop board is likely to carry. A machine has one or the other, not
 * both, and hardcoding either means networking silently does nothing on the
 * other - which is precisely what used to happen on real hardware.
 *
 * The driver is chosen once at init and reached through a small vtable, so the
 * stack above (ARP/IP/ICMP) never learns which card it is talking to. Adding a
 * third NIC is a new ops struct and one line in the probe order. */

#include "netif.h"
#include "e1000.h"
#include "rtl8169.h"
#include "logging.h"

/* The operations a NIC must provide to be usable as the bound interface. */
struct netif_ops {
    const char    *name;
    int          (*init)(void);
    int          (*present)(void);
    const uint8_t *(*mac)(void);
    int          (*send)(const void *frame, uint16_t len);
    int          (*receive)(void *buf, uint16_t max_len);
};

static const struct netif_ops nic_e1000 = {
    "e1000",  e1000_init,  e1000_present,  e1000_mac,  e1000_send,  e1000_receive,
};
static const struct netif_ops nic_rtl8169 = {
    "rtl8111", rtl8169_init, rtl8169_present, rtl8169_mac, rtl8169_send, rtl8169_receive,
};

/* Probe order. e1000 first only because it is the tested one; they are mutually
   exclusive in practice, so the order is not load-bearing. */
static const struct netif_ops *const probe_order[] = { &nic_e1000, &nic_rtl8169 };

static const struct netif_ops *nic = 0;

static struct net_config cfg;
static int ready = 0;

/* MAC reported when nothing is bound. Returning a real driver's buffer here
   would mean reading a NIC that was never initialised. */
static const uint8_t mac_none[6] = { 0, 0, 0, 0, 0, 0 };

static void set4(ip4_t dst, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
}
static void cp4(ip4_t dst, const ip4_t src) {
    for (int i = 0; i < 4; i++) dst[i] = src[i];
}

int netif_init(void) {
    /* defaults: SLIRP-friendly static config so it works out of the box in
       QEMU user networking, all overridable from the shell. */
    set4(cfg.ip,      10, 0, 2, 15);   /* QEMU SLIRP assigns .15 to the guest */
    set4(cfg.mask,    255, 255, 255, 0);
    set4(cfg.gateway, 10, 0, 2, 2);    /* SLIRP gateway */
    set4(cfg.dns1,    1, 1, 1, 1);     /* Cloudflare primary */
    set4(cfg.dns2,    1, 0, 0, 1);     /* Cloudflare secondary */

    /* Bring a NIC up. This was the missing link: netif_init only ever asked
       e1000_present(), which reports a flag that e1000_init() sets at its very
       end - and nothing called e1000_init(), so the interface could never come
       up even with the card sitting on the bus.

       Each driver's init() returns 0 when its chip is simply not on the bus,
       which is the normal case for whichever one is not installed - not an
       error, and not worth logging. */
    nic = 0;
    for (unsigned i = 0; i < sizeof(probe_order) / sizeof(probe_order[0]); i++) {
        const struct netif_ops *cand = probe_order[i];
        if (cand->present() || cand->init()) { nic = cand; break; }
    }

    if (nic) klog("NETIF", SEV_OK, nic->name);
    else     klog("NETIF", SEV_WARN, "no supported NIC found (e1000 or RTL8111/8168)");

    ready = nic != 0;
    return ready;
}

int netif_ready(void) { return ready && nic && nic->present(); }
const uint8_t *netif_mac(void) { return nic ? nic->mac() : mac_none; }
const struct net_config *netif_cfg(void) { return &cfg; }

void netif_set_ip(const ip4_t ip)        { cp4(cfg.ip, ip); }
void netif_set_mask(const ip4_t mask)    { cp4(cfg.mask, mask); }
void netif_set_gateway(const ip4_t gw)   { cp4(cfg.gateway, gw); }
void netif_set_dns(const ip4_t dns1, const ip4_t dns2) {
    cp4(cfg.dns1, dns1); cp4(cfg.dns2, dns2);
}

int netif_send(const void *frame, uint16_t len) {
    return nic ? nic->send(frame, len) : -1;
}
int netif_receive(void *buf, uint16_t max_len) {
    return nic ? nic->receive(buf, max_len) : 0;
}

/* parse "a.b.c.d" -> out[4]. needs 4 dot-separated numeric fields; we take the
   low byte of each (no range validation per design - user's responsibility). */
int netif_parse_ip(const char *s, ip4_t out) {
    int part = 0;
    uint32_t val = 0;
    int digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            digits++;
        } else if (*p == '.' || *p == '\0' || *p == ' ') {
            if (digits == 0) return 0;            /* empty field */
            if (part > 3) return 0;               /* too many parts */
            out[part++] = (uint8_t)(val & 0xFF);  /* store low byte */
            val = 0; digits = 0;
            if (*p == '\0' || *p == ' ') break;
        } else {
            return 0;                             /* non-numeric junk */
        }
    }
    return (part == 4) ? 1 : 0;
}

/* format ip -> "a.b.c.d" */
void netif_ip_str(const ip4_t ip, char *buf) {
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        /* itoa for 0..255 */
        if (v >= 100) { buf[pos++] = '0' + v / 100; v %= 100; buf[pos++] = '0' + v / 10; buf[pos++] = '0' + v % 10; }
        else if (v >= 10) { buf[pos++] = '0' + v / 10; buf[pos++] = '0' + v % 10; }
        else { buf[pos++] = '0' + v; }
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}