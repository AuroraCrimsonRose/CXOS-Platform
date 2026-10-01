/* /kernel/lib/time/datetime.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Pure calendar <-> epoch conversion (see datetime.h). */

#include "datetime.h"

static int is_leap(uint32_t y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

static uint32_t days_in_month(uint32_t y, uint32_t m /*1-12*/) {
    if (m == 2 && is_leap(y)) return 29;
    return mdays[m - 1];
}

uint64_t datetime_to_epoch(const struct datetime *dt) {
    if (!dt || dt->year < 1970 || dt->month < 1 || dt->month > 12 ||
        dt->day < 1 || dt->day > 31)
        return 0;

    uint64_t days = 0;
    for (uint32_t y = 1970; y < dt->year; y++)
        days += is_leap(y) ? 366 : 365;
    for (uint32_t m = 1; m < dt->month; m++)
        days += days_in_month(dt->year, m);
    days += (uint64_t)(dt->day - 1);

    return days * 86400ull
         + (uint64_t)dt->hour * 3600
         + (uint64_t)dt->minute * 60
         + (uint64_t)dt->second;
}

/* 64-bit / 32-bit division without libgcc (__udivdi3/__umoddi3). On 32-bit x86
   gcc would emit calls to those for `uint64_t / uint32_t`; a freestanding kernel
   has no libgcc, so we long-divide using only 32-bit-safe operations. Returns
   the quotient; *rem gets the remainder. */
static uint64_t udiv64_32(uint64_t n, uint32_t d, uint32_t *rem) {
    uint64_t q = 0;
    uint32_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | (uint32_t)((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}

void datetime_from_epoch(uint64_t epoch, struct datetime *out) {
    if (!out) return;

    uint32_t rem;
    uint64_t days = udiv64_32(epoch, 86400u, &rem);

    out->hour   = (uint8_t)(rem / 3600);
    out->minute = (uint8_t)((rem % 3600) / 60);
    out->second = (uint8_t)(rem % 60);

    uint32_t year = 1970;
    for (;;) {
        uint32_t yd = is_leap(year) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        year++;
    }
    out->year = (uint16_t)year;

    uint32_t month = 1;
    for (;;) {
        uint32_t md = days_in_month(year, month);
        if (days < md) break;
        days -= md;
        month++;
    }
    out->month = (uint8_t)month;
    out->day   = (uint8_t)(days + 1);
}