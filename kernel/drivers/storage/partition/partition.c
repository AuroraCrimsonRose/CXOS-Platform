// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/storage/partition/partition.c */
/* Aurora Tejeda / CATX Systems */
/* XBPT backend for the partition layer. See partition.h for the format. */

#include "partition.h"
#include "disk.h"

static const uint8_t XBPT_MAGIC[4] = { 'X', 'B', 'P', 'T' };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

int part_scan(uint8_t id, struct partition *out, int max, int *count) {
    if (count) *count = 0;
    if (!out || max <= 0) return -1;

    uint8_t sec[512];
    if (disk_read(id, XBPT_LBA, 1, sec) != 0) return -2;

    for (int i = 0; i < 4; i++)
        if (sec[i] != XBPT_MAGIC[i]) return -3;            /* not an XBPT disk */
    if (rd16(sec + 4) != XBPT_VERSION) return -4;

    uint16_t n  = rd16(sec + 6);
    uint16_t es = rd16(sec + 8);
    if (es != XBPT_ENTRY_SIZE)  return -5;                  /* unknown layout */
    if (n  > XBPT_MAX_ENTRIES)  n = XBPT_MAX_ENTRIES;       /* clamp to sector */

    int found = 0;
    for (uint16_t i = 0; i < n && found < max; i++) {
        const uint8_t *e = sec + XBPT_HEADER_SIZE + (uint32_t)i * XBPT_ENTRY_SIZE;
        uint8_t type = e[16];
        if (type == PART_TYPE_EMPTY) continue;              /* skip holes */

        struct partition *p = &out[found++];
        p->start_lba = rd64(e + 0);
        p->sectors   = rd64(e + 8);
        p->type      = type;
        p->flags     = e[17];
        for (int k = 0; k < 12; k++) p->name[k] = (char)e[20 + k];
        p->name[12] = '\0';
    }

    if (count) *count = found;
    return 0;
}

int part_find_type(uint8_t id, uint8_t type, struct partition *out) {
    struct partition parts[XBPT_MAX_ENTRIES];
    int n = 0;
    if (part_scan(id, parts, XBPT_MAX_ENTRIES, &n) != 0) return -1;
    for (int i = 0; i < n; i++) {
        if (parts[i].type == type) {
            if (out) *out = parts[i];
            return 0;
        }
    }
    return -2;   /* not found */
}