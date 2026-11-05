// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/hwfmt.c */
/* Aurora Tejeda / CATX Systems */
/* The hardware report's fixed form (hwfmt.h). Firmware tables are read a
   byte at a time: nothing here assumes an entry is aligned, or that a length
   the firmware wrote is true. */

#include "diag/hwfmt.h"
#include "diag/crashrec.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* Within the room a line may use; the trailer's writes pass `room` = cap. */
static void put(struct hw_out *o, const char *s, uint32_t n, uint32_t room) {
    if (o->cut) return;
    if (o->len + n > room) { o->cut = 1; return; }
    for (uint32_t i = 0; i < n; i++) o->buf[o->len++] = s[i];
}

static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static uint32_t line_room(const struct hw_out *o) {
    return o->cap > HW_TRAILER_ROOM ? o->cap - HW_TRAILER_ROOM : 0;
}

void hw_line(struct hw_out *o, const char *section) {
    o->mark = o->len;
    o->cut = 0;
    put(o, "@hw\t", 4, line_room(o));
    put(o, section, slen(section), line_room(o));
}

void hw_s(struct hw_out *o, const char *s) {
    put(o, "\t", 1, line_room(o));
    put(o, s, slen(s), line_room(o));
}

void hw_sn(struct hw_out *o, const char *s, uint32_t n) {
    char t[80];
    uint32_t k = 0, keep = 0;
    if (n > sizeof t - 1) n = sizeof t - 1;
    for (uint32_t i = 0; i < n && s[i]; i++) {
        char c = s[i];
        t[k++] = (c < 0x20 || c > 0x7E) ? '.' : c;
        if (c != ' ') keep = k;
    }
    if (keep == 0) { t[0] = '-'; keep = 1; }
    t[keep] = 0;
    hw_s(o, t);
}

void hw_x(struct hw_out *o, uint64_t v, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    char t[17];
    if (digits < 1) digits = 1;
    if (digits > 16) digits = 16;
    for (unsigned i = 0; i < digits; i++) t[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xF];
    t[digits] = 0;
    hw_s(o, t);
}

void hw_u(struct hw_out *o, uint64_t v) {
    char t[21];
    int i = 20;
    t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    hw_s(o, t + i);
}

void hw_done(struct hw_out *o) {
    put(o, "\n", 1, line_room(o));
    if (o->cut) { o->len = o->mark; o->over = 1; o->cut = 0; return; }
    o->lines++;
}

void hw_open(struct hw_out *o, char *buf, uint32_t cap) {
    o->buf = buf; o->cap = cap; o->len = 0;
    o->lines = 0; o->mark = 0; o->cut = 0; o->over = 0;
    hw_line(o, "begin");
    hw_u(o, HW_FORM_VERSION);
    hw_done(o);
}

uint32_t hw_close(struct hw_out *o) {
    o->cut = 0;
    if (o->over) { put(o, "@hw\ttruncated\n", 14, o->cap); o->lines++; }
    uint32_t crc = xcrl_crc32(0, o->buf, o->len);
    /* The end line is written as any other, but into the room kept for it. */
    uint32_t keep = o->cap;
    o->cap = o->len + HW_TRAILER_ROOM <= keep ? o->len + HW_TRAILER_ROOM : keep;
    uint32_t lines = o->lines;
    o->mark = o->len;
    put(o, "@hw\tend", 7, o->cap);
    char t[21]; int i = 20; t[i] = 0;
    uint32_t v = lines;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(o, "\t", 1, o->cap); put(o, t + i, slen(t + i), o->cap);
    static const char hex[] = "0123456789abcdef";
    char h[9];
    for (int k = 0; k < 8; k++) h[k] = hex[(crc >> (28 - 4 * k)) & 0xF];
    h[8] = 0;
    put(o, "\t", 1, o->cap); put(o, h, 8, o->cap);
    put(o, "\n", 1, o->cap);
    o->cap = keep;
    return o->len;
}

/* ---- the check ---- */

static int starts(const char *t, uint32_t n, const char *p) {
    uint32_t k = slen(p);
    if (n < k) return 0;
    for (uint32_t i = 0; i < k; i++) if (t[i] != p[i]) return 0;
    return 1;
}

int hw_check(const char *text, uint32_t len) {
    static const char begin[] = "@hw\tbegin\t1\n";
    if (!starts(text, len, begin)) return HWR_NO_BEGIN;
    if (len == 0 || text[len - 1] != '\n') return HWR_BAD_LINE;

    uint32_t lines = 0, last = 0;
    for (uint32_t at = 0; at < len; ) {
        if (!starts(text + at, len - at, "@hw\t")) return HWR_BAD_LINE;
        uint32_t e = at;
        while (text[e] != '\n') {
            if (text[e] == '\r' || text[e] == 0) return HWR_BAD_LINE;
            e++;
        }
        last = at;
        lines++;
        at = e + 1;
    }

    /* "@hw<TAB>end<TAB>decimal<TAB>8 hex<LF>" */
    const char *p = text + last;
    uint32_t n = len - last;
    if (!starts(p, n, "@hw\tend\t")) return HWR_NO_END;
    uint32_t i = 8, count = 0, digits = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9' && digits < 10) {
        count = count * 10 + (uint32_t)(p[i] - '0'); i++; digits++;
    }
    if (digits == 0 || i >= n || p[i] != '\t') return HWR_NO_END;
    i++;
    uint32_t crc = 0;
    for (int k = 0; k < 8; k++, i++) {
        if (i >= n) return HWR_NO_END;
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return HWR_NO_END;
        crc = (crc << 4) | d;
    }
    if (i != n - 1) return HWR_NO_END;

    if (count != lines - 1) return HWR_BAD_COUNT;
    if (crc != xcrl_crc32(0, text, last)) return HWR_BAD_CRC;
    return HWR_OK;
}

/* ---- firmware tables ---- */

uint8_t hw_sum(const uint8_t *p, uint32_t n) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s;
}

static const char *sum_word(const uint8_t *p, uint32_t n) { return hw_sum(p, n) == 0 ? "ok" : "bad"; }

void hw_rsdp(struct hw_out *o, uint64_t phys, const uint8_t *r) {
    uint8_t rev = r[15];
    hw_line(o, "acpi");
    hw_s(o, "rsdp");
    hw_x(o, phys, 16);
    hw_x(o, rev, 2);
    hw_sn(o, (const char *)r + 9, 6);
    hw_x(o, rd32(r + 16), 8);
    if (rev >= 2) hw_x(o, rd64(r + 24), 16); else hw_s(o, "-");
    hw_s(o, sum_word(r, 20));
    hw_s(o, rev >= 2 ? sum_word(r, 36) : "-");
    hw_done(o);
}

#define SDT_HEADER 36u

void hw_table(struct hw_out *o, const char *via, uint64_t phys, const uint8_t *t, int whole) {
    hw_line(o, "acpi");
    hw_s(o, "table");
    hw_s(o, via);
    if (!t) {
        hw_s(o, "-"); hw_x(o, phys, 16);
        for (int i = 0; i < 5; i++) hw_s(o, "-");
        hw_s(o, "unmapped");
        hw_done(o);
        return;
    }
    uint32_t len = rd32(t + 4);
    int facs = t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'S';
    hw_sn(o, (const char *)t, 4);
    hw_x(o, phys, 16);
    hw_x(o, len, 8);
    if (facs) {
        /* No revision, OEM or checksum where an SDT has them. */
        for (int i = 0; i < 4; i++) hw_s(o, "-");
        hw_s(o, "nosum");
    } else {
        hw_x(o, t[8], 2);
        hw_sn(o, (const char *)t + 10, 6);
        hw_sn(o, (const char *)t + 16, 8);
        hw_x(o, rd32(t + 24), 8);
        if (len < SDT_HEADER)  hw_s(o, "short");
        else if (!whole)       hw_s(o, "unchecked");
        else                   hw_s(o, sum_word(t, len));
    }
    hw_done(o);
}

#define MADT_HEAD 44u

static void madt_bad(struct hw_out *o, uint32_t off, int type, uint32_t len) {
    hw_line(o, "madt");
    hw_s(o, "bad");
    hw_x(o, off, 8);
    if (type < 0) hw_s(o, "-"); else hw_x(o, (uint64_t)type, 2);
    hw_x(o, len, 8);
    hw_done(o);
}

/* The length each known entry type needs; 0 for a type not read here. */
static uint32_t madt_need(uint8_t type) {
    switch (type) {
    case 0x0: return 8;     /* processor local APIC */
    case 0x1: return 12;    /* I/O APIC */
    case 0x2: return 10;    /* interrupt source override */
    case 0x4: return 6;     /* local APIC NMI */
    case 0x5: return 12;    /* local APIC address override */
    case 0x9: return 16;    /* processor local x2APIC */
    case 0xA: return 12;    /* local x2APIC NMI */
    default:  return 0;
    }
}

void hw_madt(struct hw_out *o, const uint8_t *m, uint32_t len) {
    if (len < MADT_HEAD) { madt_bad(o, 0, -1, len); return; }
    hw_line(o, "madt");
    hw_s(o, "head");
    hw_x(o, rd32(m + 36), 8);
    hw_x(o, rd32(m + 40), 8);
    hw_done(o);

    for (uint32_t off = MADT_HEAD; off < len; ) {
        if (len - off < 2) { madt_bad(o, off, m[off], 0); return; }
        uint8_t type = m[off];
        uint32_t el = m[off + 1];
        if (el < 2 || el > len - off) { madt_bad(o, off, type, el); return; }
        const uint8_t *e = m + off;
        uint32_t need = madt_need(type);
        if (need && el < need) { madt_bad(o, off, type, el); off += el; continue; }

        hw_line(o, "madt");
        switch (type) {
        case 0x0:
            hw_s(o, "cpu"); hw_x(o, e[2], 2); hw_x(o, e[3], 2); hw_x(o, rd32(e + 4), 8);
            break;
        case 0x1:
            hw_s(o, "ioapic"); hw_x(o, e[2], 2); hw_x(o, rd32(e + 4), 8); hw_x(o, rd32(e + 8), 8);
            break;
        case 0x2:
            hw_s(o, "iso"); hw_x(o, e[2], 2); hw_x(o, e[3], 2); hw_x(o, rd32(e + 4), 8); hw_x(o, rd16(e + 8), 4);
            break;
        case 0x4:
            hw_s(o, "nmi"); hw_x(o, e[2], 2); hw_x(o, rd16(e + 3), 4); hw_x(o, e[5], 2);
            break;
        case 0x5:
            hw_s(o, "lapicaddr"); hw_x(o, rd64(e + 4), 16);
            break;
        case 0x9:
            hw_s(o, "x2cpu"); hw_x(o, rd32(e + 12), 8); hw_x(o, rd32(e + 4), 8); hw_x(o, rd32(e + 8), 8);
            break;
        case 0xA:
            hw_s(o, "x2nmi"); hw_x(o, rd32(e + 4), 8); hw_x(o, rd16(e + 2), 4); hw_x(o, e[8], 2);
            break;
        default:
            hw_s(o, "other"); hw_x(o, type, 2); hw_x(o, el, 2);
            break;
        }
        hw_done(o);
        off += el;
    }
}
