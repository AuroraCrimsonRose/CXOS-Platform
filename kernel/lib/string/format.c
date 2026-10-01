/* /kernel/lib/string/format.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Pure number -> string formatting (see format.h). */

#include "format.h"

size_t fmt_u32(char *out, uint32_t v) {
    char tmp[10];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    /* tmp holds digits least-significant-first; reverse into out */
    int n = i;
    for (int j = 0; j < n; j++) out[j] = tmp[n - 1 - j];
    out[n] = '\0';
    return (size_t)n;
}

size_t fmt_i32(char *out, int32_t v) {
    if (v < 0) {
        out[0] = '-';
        /* careful with INT32_MIN: cast to unsigned via negation in 32-bit */
        uint32_t mag = (uint32_t)(-(int64_t)v);
        return 1 + fmt_u32(out + 1, mag);
    }
    return fmt_u32(out, (uint32_t)v);
}

size_t fmt_hex(char *out, uint32_t v, int width, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[8];
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v) { tmp[i++] = digits[v & 0xF]; v >>= 4; } }

    int n = i;
    int pad = 0;
    if (width > 8) width = 8;
    if (width > n) pad = width - n;   /* leading zeros to reach `width` */

    int pos = 0;
    for (int j = 0; j < pad; j++) out[pos++] = '0';
    for (int j = 0; j < n; j++) out[pos++] = tmp[n - 1 - j];
    out[pos] = '\0';
    return (size_t)pos;
}