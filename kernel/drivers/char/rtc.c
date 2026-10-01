/* /CXLite/kernel/drivers/rtc.c */
/* Aurora Tejeda */
/* CMOS RTC reader - ports 0x70 (register select) / 0x71 (data). */

#include "rtc.h"
#include "io.h"

#define CMOS_SELECT 0x70
#define CMOS_DATA   0x71

/* CMOS register numbers */
#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32   /* not always present */
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_SELECT, reg);
    return inb(CMOS_DATA);
}

/* is an RTC update in progress? (status A, bit 7) - don't read during one */
static int rtc_updating(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

/* convert BCD (e.g. 0x59 = 59) to binary */
static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

void rtc_read(struct rtc_time *out) {
    /* wait for any in-progress update to finish before reading */
    while (rtc_updating()) { }

    uint8_t sec, min, hour, day, mon, yr, cent = 0;
    uint8_t last_sec, last_min, last_hour, last_day, last_mon, last_yr, last_cent;

    /* read once */
    sec  = cmos_read(RTC_SECONDS);
    min  = cmos_read(RTC_MINUTES);
    hour = cmos_read(RTC_HOURS);
    day  = cmos_read(RTC_DAY);
    mon  = cmos_read(RTC_MONTH);
    yr   = cmos_read(RTC_YEAR);
    cent = cmos_read(RTC_CENTURY);

    /* read repeatedly until two consecutive reads agree (consistent snapshot,
       i.e. the clock didn't tick over in the middle of our reads) */
    do {
        last_sec = sec; last_min = min; last_hour = hour;
        last_day = day; last_mon = mon; last_yr = yr; last_cent = cent;

        while (rtc_updating()) { }
        sec  = cmos_read(RTC_SECONDS);
        min  = cmos_read(RTC_MINUTES);
        hour = cmos_read(RTC_HOURS);
        day  = cmos_read(RTC_DAY);
        mon  = cmos_read(RTC_MONTH);
        yr   = cmos_read(RTC_YEAR);
        cent = cmos_read(RTC_CENTURY);
    } while (sec != last_sec || min != last_min || hour != last_hour ||
             day != last_day || mon != last_mon || yr != last_yr ||
             cent != last_cent);

    /* status B: bit 2 = binary mode (else BCD), bit 1 = 24-hour (else 12) */
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int binary   = status_b & 0x04;
    int hour24   = status_b & 0x02;

    if (!binary) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        /* hour: preserve the high bit (PM flag in 12-hour BCD) during convert */
        hour = (uint8_t)(((hour & 0x0F) + (((hour & 0x70) >> 4) * 10)) | (hour & 0x80));
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
        cent = bcd_to_bin(cent);
    }

    /* 12-hour -> 24-hour if needed (bit 0x80 of hour = PM) */
    if (!hour24 && (hour & 0x80)) {
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    out->second = sec;
    out->minute = min;
    out->hour   = hour;
    out->day    = day;
    out->month  = mon;

    /* year: use century register if it looks valid, else assume 20xx */
    if (cent >= 19 && cent <= 21) out->year = (uint16_t)(cent * 100 + yr);
    else                          out->year = (uint16_t)(2000 + yr);
}