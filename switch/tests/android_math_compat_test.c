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

    // Reject wrong version, wrong provider and non-ABI-compatible symbols.
    assert(!android_math_compat_resolve("sinf", "LIBM", "libm.so", &address));
    assert(!android_math_compat_resolve("sinf", "LIBC", "libc.so", &address));
    assert(!android_math_compat_resolve("sinf", NULL, "libm.so", &address));
    assert(!android_math_compat_resolve("sinf", "LIBC", NULL, &address));
    assert(!android_math_compat_resolve("stderr", "LIBC", "libc.so", &address));
    assert(!android_math_compat_resolve("powl", "LIBC", "libm.so", &address));
    assert(!android_math_compat_resolve("sincosf", "LIBC", "libm.so", &address));
    assert(!android_math_compat_resolve("sinf", "LIBC", "libm.so", NULL));

    puts("android_math_compat_test: 13 checks passed");
    return 0;
}
