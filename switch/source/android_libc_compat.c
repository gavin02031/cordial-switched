#define _POSIX_C_SOURCE 200809L
#include "android_libc_compat.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Android and newlib use different numeric clock IDs. The relevant Android
// constants are 0=REALTIME, 1=MONOTONIC, 2=PROCESS and 3=THREAD CPU time.
// A direct function-pointer bind could quietly return the wrong time base.
// See NaGaa95/angrybirds2_nx/source/imports.c for prior Switch port practice.
static int android_clock_gettime(int android_clock, struct timespec *time_value)
{
    int host_clock;
    switch (android_clock) {
    case 0: host_clock = CLOCK_REALTIME; break;
    case 1: host_clock = CLOCK_MONOTONIC; break;
#ifdef CLOCK_PROCESS_CPUTIME_ID
    case 2: host_clock = CLOCK_PROCESS_CPUTIME_ID; break;
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
    case 3: host_clock = CLOCK_THREAD_CPUTIME_ID; break;
#endif
    default:
        errno = EINVAL;
        return -1;
    }
    if (!time_value) {
        errno = EINVAL;
        return -1;
    }
    return clock_gettime(host_clock, time_value);
}

bool android_libc_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
    // C routines with an equivalent AArch64 calling convention only. No
    // pthread objects, errno state, locale objects or fortify signatures:
    // those need proper Bionic adapters and may not alias native structs.
#define MAP_LIBC(symbol) \
    if (strcmp(name, #symbol) == 0) *address = (uint64_t)(uintptr_t)&symbol; else
    MAP_LIBC(strlen)
    MAP_LIBC(strcmp)
    MAP_LIBC(strncmp)
    MAP_LIBC(strchr)
    MAP_LIBC(strrchr)
    MAP_LIBC(strstr)
    MAP_LIBC(memcmp)
    MAP_LIBC(memchr)
    MAP_LIBC(memcpy)
    MAP_LIBC(memmove)
    MAP_LIBC(memset)
    MAP_LIBC(strcpy)
    MAP_LIBC(strncpy)
    MAP_LIBC(vsnprintf)
    MAP_LIBC(snprintf)
    MAP_LIBC(strtol)
    MAP_LIBC(strtoul)
    MAP_LIBC(strtod)
    MAP_LIBC(strtof)
    MAP_LIBC(atoi)
    MAP_LIBC(atol)
    MAP_LIBC(abs)
    MAP_LIBC(rand)
    MAP_LIBC(srand)
    MAP_LIBC(qsort)
    MAP_LIBC(bsearch)
    if (strcmp(name, "clock_gettime") == 0)
        *address = (uint64_t)(uintptr_t)&android_clock_gettime;
#undef MAP_LIBC
    return *address != 0;
}
