// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#include <stdint.h>
#include <stdbool.h>
typedef unsigned __int128 u128;
struct wide { uint64_t a; uint32_t b; };
struct big { uint64_t a; uint32_t b; uint64_t c; uint32_t d; };
_Static_assert(sizeof(struct wide) == 16 && sizeof(struct big) == 32, "the layouts x.xfxn assumes");
uint64_t x_many(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
u128 x_skip(uint64_t a, u128 b, uint64_t c, uint64_t d, u128 f, uint64_t g);
struct wide x_wide(struct wide w, uint32_t k);
struct big x_big(struct big b, uint64_t k);
uint64_t x_after_big(struct big b, uint64_t x, uint64_t y);
u128 x_late(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, u128 h);
uint32_t x_calls_c(void);

uint64_t c_many(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
}
u128 c_skip(uint64_t a, u128 b, uint64_t c, uint64_t d, u128 f, uint64_t g) {
    return (u128)a + b * 3 + (u128)(c + d) + f * 5 + ((u128)g << 64);
}
struct wide c_wide(struct wide w, uint32_t k) { struct wide r = { w.a + 2, w.b + k * 2 }; return r; }
struct big c_big(struct big b, uint64_t k) { struct big r = { b.a + k, b.b + 1, b.c * 3, b.d * 2 }; return r; }
uint64_t c_after_big(struct big b, uint64_t x, uint64_t y) { return b.c + x * 10 + y * 100; }
u128 c_late(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, u128 h) {
    return (u128)(a + b + c + d + e + f) + (u128)g * 1000 + h;
}
uint32_t *c_next(uint32_t *p) { return p + 1; }
bool c_not(bool b) { return !b; }

int main(void) {
    if (x_many(1, 2, 3, 4, 5, 6, 7, 8) != 204) return 1;
    u128 b = ((u128)2 << 64) | 3, f = ((u128)6 << 64) | 7;
    if (x_skip(1, b, 4, 5, f, 8) != ((u128)44 << 64) + 54) return 2;
    struct wide w = { 0x100000000ull, 7 };
    struct wide v = x_wide(w, 5);
    if (v.a != 0x100000001ull || v.b != 12) return 3;
    struct big g = { 1, 2, 0x300000000ull, 4 };
    struct big h = x_big(g, 10);
    if (h.a != 11 || h.b != 3 || h.c != 0x600000000ull || h.d != 6) return 4;
    if (x_after_big(g, 5, 6) != 0x300000000ull + 50 + 600) return 5;
    if (x_late(1, 2, 3, 4, 5, 6, 7, ((u128)9 << 64) | 5) != ((u128)9 << 64) + 7026) return 6;
    uint32_t r = x_calls_c();
    if (r) return 10 + r;
    return 0;
}
