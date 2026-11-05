// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/hwfmt.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The hardware report's text (roadmap 1.8, docs/hardware/CX_HW_REPORT.md):
 * the fixed form, and what reads a firmware table into it. No hardware here,
 * so it is tested on the host (kernel/test/host/test_hwfmt.c); hwreport.c
 * gathers what the machine has and hands it to these.
 *
 * ---- the form --------------------------------------------------------------
 * One line per fact, fields separated by a tab, each line "@hw<TAB>section"
 * then its fields, ending in a newline (LF; serial adds CR). Numbers are
 * lower-case hex at a fixed width unless a field says decimal; a text field
 * keeps printable ASCII, turns anything else into '.', drops trailing spaces,
 * and is "-" when nothing is left. The first line is "@hw begin 1" (the form's
 * version), the last "@hw end <lines> <crc>": how many lines came before it,
 * and the CRC-32 (xcrl_crc32) of every byte before it. A report that ran out
 * of room ends "@hw truncated" before the end line, so a cut report still
 * checks and says it was cut.
 */

#ifndef DIAG_HWFMT_H
#define DIAG_HWFMT_H

#include <stdint.h>

#define HW_FORM_VERSION  1u
#define HW_TRAILER_ROOM  64u    /* kept free for "truncated" and the end line */

struct hw_out {
    char    *buf;
    uint32_t cap, len;
    uint32_t lines;      /* finished lines, the begin line included */
    uint32_t mark;       /* where the line being written started */
    int      cut;        /* the line being written ran out of room */
    int      over;       /* a line did not fit and was dropped */
};

/* Start a report in buf (cap bytes): writes the begin line. */
void hw_open(struct hw_out *o, char *buf, uint32_t cap);
/* Start a line: "@hw<TAB>section". */
void hw_line(struct hw_out *o, const char *section);
/* Add a field: a string as it is (the caller's own words). */
void hw_s(struct hw_out *o, const char *s);
/* Add a field: at most n bytes of firmware text, stopping at a NUL, cleaned
   as the form says. */
void hw_sn(struct hw_out *o, const char *s, uint32_t n);
/* Add a field: v in `digits` hex digits (1-16), the low ones if it is wider. */
void hw_x(struct hw_out *o, uint64_t v, unsigned digits);
/* Add a field: v in decimal. */
void hw_u(struct hw_out *o, uint64_t v);
/* End the line. A line that did not fit (with HW_TRAILER_ROOM to spare) is
   taken back whole and the report marked cut. */
void hw_done(struct hw_out *o);
/* Write the trailer; the report's length. Nothing may be added after. */
uint32_t hw_close(struct hw_out *o);

/* What hw_check made of a report. */
#define HWR_OK         0
#define HWR_NO_BEGIN  -1   /* not "@hw begin <HW_FORM_VERSION>" first */
#define HWR_BAD_LINE  -2   /* a line not "@hw<TAB>...", a CR, a NUL, or no LF */
#define HWR_NO_END    -3   /* the last line is not "@hw end <n> <crc>" */
#define HWR_BAD_COUNT -4
#define HWR_BAD_CRC   -5

/* Check a whole report, exactly len bytes, ending with the end line. */
int hw_check(const char *text, uint32_t len);

/* The byte sum ACPI checksums are judged by: 0 for a good table. */
uint8_t hw_sum(const uint8_t *p, uint32_t n);

/* The RSDP, 20 bytes, or 36 when its revision is 2 or more (the caller maps
   36): "acpi rsdp <phys> <rev> <oem> <rsdt> <xsdt|-> <sum> <xsum|->", each
   sum "ok" or "bad". */
void hw_rsdp(struct hw_out *o, uint64_t phys, const uint8_t *r);

/* One table, found through `via` ("rsdt", "xsdt", "fadt"):
   "acpi table <via> <sig> <phys> <len> <rev> <oem> <oem-table> <oem-rev> <state>".
   `t` is the table, or 0 when its header could not be mapped (state
   "unmapped", the rest "-"); `whole` says the caller mapped all of its
   length (else "unchecked": a length the caller would not map). state is
   "ok" or "bad" by its checksum, "short" for a length under the header's,
   and "nosum" for the FACS, which has none. */
void hw_table(struct hw_out *o, const char *via, uint64_t phys, const uint8_t *t, int whole);

/* The MADT (`len` bytes, its own length field ignored for the walk's
   bound): "madt head <lapic-addr> <flags>", then a line per entry:
     cpu <uid> <apic-id> <flags>          ioapic <id> <addr> <gsi-base>
     iso <bus> <source> <gsi> <flags>     nmi <uid> <flags> <lint>
     lapicaddr <addr>                     x2cpu <uid> <x2apic-id> <flags>
     x2nmi <uid> <flags> <lint>           other <type> <len>
   and "madt bad <offset> <type> <len>" for an entry too short for its type
   (skipped by its length), or one whose length is under 2 or runs past the
   end (the walk stops there). A table under the 44-byte head gives only
   "madt bad 0 - <len>". */
void hw_madt(struct hw_out *o, const uint8_t *m, uint32_t len);

#endif
