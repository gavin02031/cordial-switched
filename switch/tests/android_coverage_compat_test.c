#include "android_coverage_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint64_t dump = 0, flush = 0;
    assert(android_coverage_compat_resolve("__gcov_dump", NULL, NULL, &dump));
    assert(android_coverage_compat_resolve("__gcov_flush", NULL, NULL, &flush));
    assert(dump && dump == flush);
    void (*diagnose)(void) = (void (*)(void))(uintptr_t)dump;
    diagnose();
    diagnose();
    assert(!android_coverage_compat_resolve("__gcov_dump", "LIBC", "libc.so", &dump));
    assert(!android_coverage_compat_resolve("__gcov_dump", NULL, "libc.so", &dump));
    assert(!android_coverage_compat_resolve("gcov_random_unknown", NULL, NULL, &dump));
    assert(!android_coverage_compat_resolve("__gcov_dump", NULL, NULL, NULL));
    puts("android_coverage_compat_test: diagnostic-only gcov hooks passed");
    return 0;
}
