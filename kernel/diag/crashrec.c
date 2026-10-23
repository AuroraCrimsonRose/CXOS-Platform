// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/crashrec.c */
/* Aurora Tejeda / CATX Systems */
/* The XCRL crash record. See crashrec.h for the layout. */

#include "diag/crashrec.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Bit at a time: no table, so nothing to get wrong at panic and no 1 KB of
   .rodata for a few kilobytes hashed once per crash. */
uint32_t xcrl_crc32(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void xcrl_seal(uint8_t hdr[XCRL_SECTOR], uint16_t kind, const char *text, uint32_t len) {
    for (uint32_t i = 0; i < XCRL_SECTOR; i++) hdr[i] = 0;
    if (len > XCRL_TEXT_MAX) len = XCRL_TEXT_MAX;
    wr32(hdr + 0, XCRL_MAGIC);
    hdr[4] = (uint8_t)XCRL_VERSION; hdr[5] = (uint8_t)(XCRL_VERSION >> 8);
    hdr[6] = (uint8_t)kind;         hdr[7] = (uint8_t)(kind >> 8);
    wr32(hdr + 8, len);
    uint32_t crc = xcrl_crc32(0, hdr, 12);
    if (len) crc = xcrl_crc32(crc, text, len);
    wr32(hdr + 12, crc);
}

int xcrl_check_header(const uint8_t hdr[XCRL_SECTOR], uint16_t *kind, uint32_t *len) {
    if (rd32(hdr) != XCRL_MAGIC) return XCRL_EMPTY;
    uint16_t ver = (uint16_t)(hdr[4] | (hdr[5] << 8));
    uint16_t k   = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint32_t n   = rd32(hdr + 8);
    if (ver != XCRL_VERSION) return XCRL_BAD_VER;
    if (k != XCRL_KIND_CRASH && k != XCRL_KIND_REQUEST && k != XCRL_KIND_FAULT) return XCRL_BAD_KIND;
    if (n > XCRL_TEXT_MAX || (k == XCRL_KIND_REQUEST && n != 0)) return XCRL_BAD_LEN;
    if (kind) *kind = k;
    if (len)  *len = n;
    return XCRL_VALID;
}

int xcrl_check_text(const uint8_t hdr[XCRL_SECTOR], const char *text, uint32_t len) {
    uint32_t crc = xcrl_crc32(0, hdr, 12);
    if (len) crc = xcrl_crc32(crc, text, len);
    return crc == rd32(hdr + 12) ? XCRL_VALID : XCRL_BAD_CRC;
}
