#include "android_math_compat.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint64_t address = 0;
    assert(android_math_compat_resolve("sinf", "LIBC", "libm.so", &address));
    assert(address);
    float (*sine)(float) = (float (*)(float))(uintptr_t)address;
    assert(fabsf(sine(0.5f) - sinf(0.5f)) < 0.00001f);

    assert(android_math_compat_resolve("pow", "LIBC", "libm.so", &address));
    double (*power)(double, double) = (double (*)(double, double))(uintptr_t)address;
    assert(power(2.0, 3.0) == 8.0);

    assert(android_math_compat_resolve("ldexp", "LIBC", "libm.so", &address));
    double (*scale)(double, int) = (double (*)(double, int))(uintptr_t)address;
    assert(scale(1.5, 3) == 12.0);
    assert(android_math_compat_resolve("round", "LIBC", "libm.so", &address));
    double (*nearest)(double) = (double (*)(double))(uintptr_t)address;
    assert(nearest(-1.5) == -2.0);

    // Reject wrong version, wrong provider and non-ABI-compatible symbols.
    assert(!android_math_compat_resolve("sinf", "LIBM", "libm.so", &address));
    assert(!android_math_compat_resolve("sinf", "LIBC", "libc.so", &address));
    assert(!android_math_compat_resolve("sinf", NULL, "libm.so", &address));
    assert(!android_math_compat_resolve("sinf", "LIBC", NULL, &address));
    assert(!android_math_compat_resolve("stderr", "LIBC", "libc.so", &address));
    assert(android_math_compat_resolve("powl", "LIBC", "libm.so", &address));
    long double (*powerl)(long double, long double) =
        (long double (*)(long double, long double))(uintptr_t)address;
    assert(powerl(2.0L, 4.0L) == 16.0L);
    assert(android_math_compat_resolve("fmal", "LIBC", "libm.so", &address));
    long double (*fused)(long double, long double, long double) =
        (long double (*)(long double, long double, long double))(uintptr_t)address;
    assert(fused(2.0L, 3.0L, 1.0L) == 7.0L);
    assert(android_math_compat_resolve("roundl", "LIBC", "libm.so", &address));
    long double (*round_long)(long double) =
        (long double (*)(long double))(uintptr_t)address;
    assert(round_long(-2.5L) == -3.0L);
    assert(android_math_compat_resolve("llroundl", "LIBC", "libm.so", &address));
    long long (*long_integer)(long double) =
        (long long (*)(long double))(uintptr_t)address;
    assert(long_integer(-2.5L) == -3);
    assert(!android_math_compat_resolve("powl", "LIBC_O", "libm.so", &address));
    assert(!android_math_compat_resolve("powl", "LIBC", "libc.so", &address));
    assert(android_math_compat_resolve("sincosf", "LIBC", "libm.so", &address));
    void (*sincos_pair_f)(float, float *, float *) =
        (void (*)(float, float *, float *))(uintptr_t)address;
    float s = 0, c = 0;
    sincos_pair_f(0.5f, &s, &c);
    assert(fabsf(s - sinf(0.5f)) < 0.00001f &&
           fabsf(c - cosf(0.5f)) < 0.00001f);
    assert(android_math_compat_resolve("sincos", "LIBC", "libm.so", &address));
    void (*sincos_pair)(double, double *, double *) =
        (void (*)(double, double *, double *))(uintptr_t)address;
    double ds = 0, dc = 0;
    sincos_pair(0.5, &ds, &dc);
    assert(fabs(ds - sin(0.5)) < 1e-12 && fabs(dc - cos(0.5)) < 1e-12);
    assert(android_math_compat_resolve("finitef", "LIBC", "libm.so", &address));
    int (*finite_float)(float) = (int (*)(float))(uintptr_t)address;
    assert(finite_float(1.0f) == 1 && finite_float(NAN) == 0);
    assert(!android_math_compat_resolve("sincosf", "LIBC", "libc.so", &address));
    assert(!android_math_compat_resolve("sinf", "LIBC", "libm.so", NULL));

    puts("android_math_compat_test: all assertions passed");
    return 0;
}
