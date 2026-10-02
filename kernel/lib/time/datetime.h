/* /kernel/lib/time/datetime.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Pure calendar <-> epoch conversion. No hardware: callers pass in a broken-down
 * date (e.g. read from the RTC driver) and get seconds since the CXOS epoch
 * (1970-01-01T00:00:00Z, Unix-compatible), or the reverse. Proleptic Gregorian,
 * UTC, no leap seconds.
 */

#ifndef CXK_DATETIME_H
#define CXK_DATETIME_H

#include <stdint.h>

struct datetime {
    uint16_t year;     /* full year, e.g. 2026 */
    uint8_t  month;    /* 1-12 */
    uint8_t  day;      /* 1-31 */
    uint8_t  hour;     /* 0-23 */
    uint8_t  minute;   /* 0-59 */
    uint8_t  second;   /* 0-59 */
};

/* broken-down date -> seconds since 1970-01-01T00:00:00Z. */
uint64_t datetime_to_epoch(const struct datetime *dt);

/* seconds since epoch -> broken-down date (UTC). */
void datetime_from_epoch(uint64_t epoch, struct datetime *out);

#endif