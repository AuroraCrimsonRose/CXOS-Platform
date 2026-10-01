/* /CXLite/kernel/drivers/rtc.h */
/* Aurora Tejeda */
/*
 * CMOS RTC (real-time clock) reader.
 *
 * Reads the battery-backed real-time clock via CMOS ports 0x70/0x71.
 * Unlike the PIT (which we count for uptime), the RTC keeps true wall-clock
 * time, accurate on both emulators and real hardware. Handles BCD/binary
 * formats and does a consistency check to avoid mid-tick rollover.
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_time {
    uint16_t year;     /* full year, e.g. 2026 */
    uint8_t  month;    /* 1-12 */
    uint8_t  day;      /* 1-31 */
    uint8_t  hour;     /* 0-23 (24-hour) */
    uint8_t  minute;   /* 0-59 */
    uint8_t  second;   /* 0-59 */
};

/* read the current time from the RTC into `out`. */
void rtc_read(struct rtc_time *out);

#endif