// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/math/kmath.c */
/* Aurora Tejeda */
/* Freestanding sin/cos/sqrt, in plain C (no instruction of any one CPU), so
   the host tests check them (test_kmath.c). Not in the kernel build: see
   tools/cmake/CMakeLists.txt. */

#include <stdint.h>
#include "kmath.h"

/* sqrt by Newton's method. It used to be the SSE instruction, which made this
   the one x86-only file in lib/. The first guess halves the exponent straight
   out of the bits, within a factor of 1.5 of the root for a normal number;
   each step then roughly doubles the correct digits. A subnormal starts
   further off and takes more steps. The loop ends when a step changes
   nothing, or after 64, when it is flipping between the two doubles either
   side of the root - both within one unit in the last place. */
double km_sqrt(double x) {
    if (!(x > 0.0)) return 0.0;              /* also a NaN */
    if (x > 1.7976931348623157e308) return x; /* infinity */
    union { double d; uint64_t u; } g = { x };
    g.u = (g.u >> 1) + ((uint64_t)1023 << 51);
    double r = g.d;
    for (int i = 0; i < 64; i++) {
        double n = 0.5 * (r + x / r);
        if (n == r) break;
        r = n;
    }
    return r;
}

/* Reduce the angle to [-PI/2, PI/2], where the series below is accurate. To
   [-PI, PI] first, by whole turns; then past PI/2 by sin(PI - x) = sin(x).
   Stopping at [-PI, PI] left an error of 2e-5 at the ends (the first term
   dropped, x^15/15!, at x = PI), not the 1e-9 claimed; the host tests found
   it (test_kmath.c). */
static double reduce(double x) {
    while (x >  KM_PI) x -= KM_2PI;
    while (x < -KM_PI) x += KM_2PI;
    if (x >  KM_PI / 2.0) x =  KM_PI - x;
    if (x < -KM_PI / 2.0) x = -KM_PI - x;
    return x;
}

/* sin via Taylor series around 0, to x^13/13!: accurate to ~1e-9 over
   [-PI/2, PI/2] after range reduction. */
double km_sin(double x) {
    x = reduce(x);
    double x2 = x * x;
    double term = x;          /* current term, starts at x^1/1! */
    double sum = x;
    /* term_{n+1} = term_n * (-x^2) / ((2n)(2n+1)) */
    term *= -x2 / (2.0 * 3.0);   sum += term;
    term *= -x2 / (4.0 * 5.0);   sum += term;
    term *= -x2 / (6.0 * 7.0);   sum += term;
    term *= -x2 / (8.0 * 9.0);   sum += term;
    term *= -x2 / (10.0 * 11.0); sum += term;
    term *= -x2 / (12.0 * 13.0); sum += term;
    return sum;
}

double km_cos(double x) {
    /* cos(x) = sin(x + PI/2) */
    return km_sin(x + KM_PI / 2.0);
}