/* /CXLite/kernel/drivers/netif.c */
/* Aurora Tejeda */
/* Network interface abstraction + static config. Bound to the e1000 today. */

#include "netif.h"
#include "e1000.h"

static struct net_config cfg;
static int ready = 0;

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

    ready = e1000_present();
    return ready;
}

int netif_ready(void) { return ready && e1000_present(); }
const uint8_t *netif_mac(void) { return e1000_mac(); }
const struct net_config *netif_cfg(void) { return &cfg; }

void netif_set_ip(const ip4_t ip)        { cp4(cfg.ip, ip); }
void netif_set_mask(const ip4_t mask)    { cp4(cfg.mask, mask); }
void netif_set_gateway(const ip4_t gw)   { cp4(cfg.gateway, gw); }
void netif_set_dns(const ip4_t dns1, const ip4_t dns2) {
    cp4(cfg.dns1, dns1); cp4(cfg.dns2, dns2);
}

int netif_send(const void *frame, uint16_t len)   { return e1000_send(frame, len); }
int netif_receive(void *buf, uint16_t max_len)     { return e1000_receive(buf, max_len); }

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