/* /CXLite/kernel/lib/kmath.h */
/* Aurora Tejeda */
/* Tiny freestanding math: sin, cos, sqrt. No libm in the kernel, so we roll
   our own. sin/cos use range reduction + Taylor series; sqrt uses the SSE
   hardware instruction. Requires fpu_init() to have run. */

#ifndef KMATH_H
#define KMATH_H

#define KM_PI   3.14159265358979323846
#define KM_2PI  6.28318530717958647692

double km_sin(double x);
double km_cos(double x);
double km_sqrt(double x);

#endif