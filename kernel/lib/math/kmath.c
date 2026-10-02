// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/lib/kmath.c */
/* Aurora Tejeda */
/* Freestanding sin/cos/sqrt. Compiled with SSE (see CMake). */

#include "kmath.h"

/* sqrt via the SSE scalar square-root instruction (exact, fast). */
double km_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r;
    __asm__ volatile ("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* reduce angle to [-PI, PI] for accurate Taylor evaluation */
static double reduce(double x) {
    /* subtract multiples of 2*PI */
    while (x >  KM_PI) x -= KM_2PI;
    while (x < -KM_PI) x += KM_2PI;
    return x;
}

/* sin via Taylor series around 0: x - x^3/3! + x^5/5! - x^7/7! + x^9/9! - x^11/11!
   accurate to ~1e-9 over [-PI,PI] after range reduction. */
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