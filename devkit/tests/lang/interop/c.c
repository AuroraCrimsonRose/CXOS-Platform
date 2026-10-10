// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#include <stdint.h>
struct pt { uint32_t x, y; };
uint64_t x_add(uint64_t a, uint64_t b);
struct pt x_mk(uint32_t a);
uint64_t x_sum(struct pt p, uint64_t k);
uint32_t x_calls_c(void);
uint64_t c_mul(uint64_t a, uint32_t b) { return a * b; }
struct pt c_mk(uint32_t a) { struct pt p = { a, a * 10 }; return p; }
uint64_t c_sum(struct pt p, uint64_t k) { return p.x + p.y + k; }
int main(void) {
    if (x_add(0xFFFFFFFFull, 1) != 0x100000000ull) return 1;
    struct pt p = x_mk(21);
    if (p.x != 21 || p.y != 42) return 2;
    if (x_sum(p, 0x500000000ull) != 0x50000003Full) return 3;
    uint32_t r = x_calls_c();
    if (r) return 10 + r;
    return 0;
}
