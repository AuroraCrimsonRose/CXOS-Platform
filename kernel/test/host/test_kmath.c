// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_kmath.c */
/* Aurora Tejeda / CATX Systems */
/* The kernel's freestanding math (lib/math/kmath.c), against the host's libm. */
#include "khost.h"
#include "lib/math/kmath.h"

#include <math.h>
#include <float.h>

/* within one unit in the last place */
static int ulp1(double got, double want) {
    return got == want || got == nextafter(want, INFINITY) || got == nextafter(want, -INFINITY);
}

KH_TEST(kmath_sqrt_exact_on_perfect_squares) {
    for (int i = 1; i <= 4096; i++) {
        double n = (double)i;
        KH_CHECK(km_sqrt(n * n) == n);
    }
    KH_CHECK(km_sqrt(0.25) == 0.5);
    KH_CHECK(km_sqrt(1.0) == 1.0);
}

KH_TEST(kmath_sqrt_within_an_ulp) {
    /* across the exponent range, subnormals included */
    const double xs[] = { 2.0, 3.0, 10.0, 0.1, 1e-300, 1e300, DBL_MAX, DBL_MIN,
                          4.9e-324, 1e-310, 123456789.0, 0.999999, 1.000001 };
    for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++)
        KH_CHECK(ulp1(km_sqrt(xs[i]), sqrt(xs[i])));
    for (double x = 1e-6; x < 1e6; x *= 1.37)
        KH_CHECK(ulp1(km_sqrt(x), sqrt(x)));
}

KH_TEST(kmath_sqrt_edges) {
    KH_CHECK(km_sqrt(0.0) == 0.0);
    KH_CHECK(km_sqrt(-4.0) == 0.0);
    KH_CHECK(km_sqrt(NAN) == 0.0);
    KH_CHECK(km_sqrt(INFINITY) == INFINITY);
}

KH_TEST(kmath_sin_cos_close) {
    for (double x = -20.0; x <= 20.0; x += 0.01) {
        KH_CHECK(fabs(km_sin(x) - sin(x)) < 1e-8);
        KH_CHECK(fabs(km_cos(x) - cos(x)) < 1e-8);
    }
}
