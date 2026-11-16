#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "android_libc_compat.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <wchar.h>
#include <arpa/inet.h>
#include <sys/stat.h>

// Android ARM64 and Horizon AArch64 both use 32-bit wchar_t. Only forward
// these pure wide-string operations when the native ABI actually matches.
_Static_assert(sizeof(wchar_t) == 4, "Bionic ARM64 wchar_t must be 4 bytes");
#ifdef __SWITCH__
#include <switch.h>
#else
#include <sched.h>
#endif

// Android's unversioned getentropy is not a general-purpose random() call.
// Use libnx's OS-seeded ChaCha generator on Switch and libc's getentropy in
// host tests. Both produce random bytes and never report success with zeros.
// Android/glibc reject requests above 256 bytes rather than truncating them.
static int android_getentropy(void *buffer, size_t count)
{
    if (count > 256) { errno = EIO; return -1; }
    if (!buffer && count) { errno = EFAULT; return -1; }
#ifdef __SWITCH__
    if (count) randomGet(buffer, count);
    return 0;
#else
    return getentropy(buffer, count);
#endif
}

static void android_arc4random_buf(void *buffer, size_t count)
{
    // The API returns no error code. Never pass cryptographic callers an
    // uninitialised buffer while pretending the native entropy call worked.
    if (!buffer && count) abort();
    unsigned char *target = buffer;
    while (count) {
        size_t chunk = count > 256 ? 256 : count;
        if (android_getentropy(target, chunk) != 0) abort();
        target += chunk;
        count -= chunk;
    }
}

static atomic_int stack_guard_state;
static uintptr_t stack_guard;

static bool initialise_stack_guard(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&stack_guard_state, &expected, 1)) {
        uintptr_t value = 0;
        bool seeded = android_getentropy(&value, sizeof(value)) == 0 && value != 0;
        if (seeded) stack_guard = value;
        atomic_store(&stack_guard_state, seeded ? 2 : 3);
    }
    while (atomic_load(&stack_guard_state) == 1) {
        // A parallel import audit must not observe a partially seeded guard.
    }
    return atomic_load(&stack_guard_state) == 2;
}

static void *android_memrchr(const void *data, int character, size_t count)
{
    const unsigned char *bytes = data;
    while (count) {
        --count;
        if (bytes[count] == (unsigned char)character)
            return (void *)(bytes + count);
    }
    return NULL;
}

static int android_getpagesize(void)
{
#ifdef __SWITCH__
    return 4096; // Horizon memory page size, not ELF segment alignment.
#else
    return (int)sysconf(_SC_PAGESIZE);
#endif
}

static int android_sched_yield(void)
{
#ifdef __SWITCH__
    svcSleepThread(0);
    return 0;
#else
    return sched_yield();
#endif
}

// Bionic's ARM64 msghdr/cmsghdr layout is Linux LP64, not a host BSD socket
// header. Only calculate the next ancillary-data header after checking every
// address and size against the original guest control buffer.
typedef struct {
    void *name;
    uint32_t name_length;
    void *iov;
    size_t iov_length;
    void *control;
    size_t control_length;
    int flags;
} AndroidMsghdr;

typedef struct {
    size_t length;
    int level;
    int type;
} AndroidCmsghdr;

_Static_assert(offsetof(AndroidMsghdr, control) == 32 &&
               offsetof(AndroidMsghdr, control_length) == 40 &&
               sizeof(AndroidCmsghdr) == 16, "Android LP64 socket header layout");

static AndroidCmsghdr *android_cmsg_nxthdr(const AndroidMsghdr *msg,
                                           const AndroidCmsghdr *current)
{
    if (!msg || !msg->control || !current)
        return NULL;
    const uintptr_t base = (uintptr_t)msg->control;
    const uintptr_t here = (uintptr_t)current;
    if (here < base || msg->control_length > UINTPTR_MAX - base)
        return NULL;
    const uintptr_t end = base + msg->control_length;
    if (here > end || (size_t)(end - here) < sizeof(*current) ||
        current->length < sizeof(*current) ||
        current->length > (size_t)(end - here) ||
        current->length > SIZE_MAX - 7)
        return NULL;
    const size_t stride = (current->length + 7) & ~(size_t)7;
    if (stride > (size_t)(end - here) ||
        sizeof(*current) > (size_t)(end - here) - stride)
        return NULL;
    return (AndroidCmsghdr *)(here + stride);
}

// Bionic localeconv returns the C-locale numeric format independently of the
// host's process locale. The 10 pointers + 14 signed-char slots are its LP64
// public struct lconv; no opaque newlib locale objects cross this boundary.
typedef struct {
    char *decimal_point, *thousands_sep, *grouping;
    char *int_curr_symbol, *currency_symbol, *mon_decimal_point;
    char *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space;
    char n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    char int_p_cs_precedes, int_p_sep_by_space;
    char int_n_cs_precedes, int_n_sep_by_space;
    char int_p_sign_posn, int_n_sign_posn;
} AndroidLconv;

_Static_assert(offsetof(AndroidLconv, int_frac_digits) == 80 &&
               sizeof(AndroidLconv) == 96, "Android LP64 localeconv layout");

static AndroidLconv *android_localeconv(void)
{
    static char dot[] = ".";
    static char empty[] = "";
    static AndroidLconv value = {
        .decimal_point = dot, .thousands_sep = empty, .grouping = empty,
        .int_curr_symbol = empty, .currency_symbol = empty,
        .mon_decimal_point = empty, .mon_thousands_sep = empty,
        .mon_grouping = empty, .positive_sign = empty, .negative_sign = empty,
        .int_frac_digits = CHAR_MAX, .frac_digits = CHAR_MAX,
        .p_cs_precedes = CHAR_MAX, .p_sep_by_space = CHAR_MAX,
        .n_cs_precedes = CHAR_MAX, .n_sep_by_space = CHAR_MAX,
        .p_sign_posn = CHAR_MAX, .n_sign_posn = CHAR_MAX,
        .int_p_cs_precedes = CHAR_MAX, .int_p_sep_by_space = CHAR_MAX,
        .int_n_cs_precedes = CHAR_MAX, .int_n_sep_by_space = CHAR_MAX,
        .int_p_sign_posn = CHAR_MAX, .int_n_sign_posn = CHAR_MAX,
    };
    return &value;
}

// Bionic exports these 16-byte IPv6 address constants as LIBC_N data
// symbols. A host's provider version cannot be assumed to match, but their
// byte representation is defined by RFC 4291 and has no libc struct padding.
static const _Alignas(16) unsigned char ipv6_unspecified[16] = {0};
static const _Alignas(16) unsigned char ipv6_loopback[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
};

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

// Both providers must expose the same storage used by getopt_long, getenv
// and tzset. Snapshot globals would go stale as soon as those functions run.
extern char **environ;
_Static_assert(sizeof(struct option) == 32, "Android LP64 getopt option size");
_Static_assert(offsetof(struct option, flag) == 16, "Android getopt flag offset");

bool android_libc_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address)
{
    if (name && address && !version && !library &&
        strcmp(name, "getentropy") == 0) {
        *address = (uint64_t)(uintptr_t)&android_getentropy;
        return true;
    }
    if (name && address && version && library &&
        strcmp(version, "LIBC") == 0 && strcmp(library, "libc.so") == 0 &&
        strcmp(name, "__stack_chk_guard") == 0) {
        if (!initialise_stack_guard()) return false;
        *address = (uint64_t)(uintptr_t)&stack_guard;
        return true;
    }
    if (name && address && version && library &&
        strcmp(version, "LIBC_N") == 0 &&
        strcmp(library, "libc.so") == 0) {
        if (strcmp(name, "in6addr_any") == 0)
            *address = (uint64_t)(uintptr_t)ipv6_unspecified;
        else if (strcmp(name, "in6addr_loopback") == 0)
            *address = (uint64_t)(uintptr_t)ipv6_loopback;
        else
            *address = 0;
        return *address != 0;
    }
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
#define DATA(symbol, target) if (!strcmp(name, symbol)) { *address = (uint64_t)(uintptr_t)(target); return true; }
    DATA("optarg", &optarg)
    DATA("optind", &optind)
    DATA("environ", &environ)
    DATA("tzname", tzname)
#ifdef __SWITCH__
    DATA("timezone", &_timezone)
    DATA("daylight", &_daylight)
#else
    DATA("timezone", &timezone)
    DATA("daylight", &daylight)
#endif
#undef DATA
    // C routines with an equivalent AArch64 calling convention only. Ordinary
    // file descriptors and path strings can use newlib directly; a missing
    // Android filesystem path still fails normally with ENOENT. Do not extend
    // this to stat, directory entries, flags or opaque objects without a
    // verified guest/host ABI translation.
#define MAP_LIBC(symbol) \
    if (strcmp(name, #symbol) == 0) *address = (uint64_t)(uintptr_t)&symbol; else
    MAP_LIBC(getopt_long)
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
    MAP_LIBC(strcat)
    MAP_LIBC(strncat)
    MAP_LIBC(strcspn)
    MAP_LIBC(strspn)
    MAP_LIBC(strpbrk)
    MAP_LIBC(strnlen)
    MAP_LIBC(strcasecmp)
    MAP_LIBC(strncasecmp)
    MAP_LIBC(vsnprintf)
    MAP_LIBC(snprintf)
    MAP_LIBC(strtol)
    MAP_LIBC(strtoul)
    MAP_LIBC(strtoll)
    MAP_LIBC(strtoull)
    MAP_LIBC(strtod)
    MAP_LIBC(strtof)
    MAP_LIBC(atoi)
    MAP_LIBC(atol)
    MAP_LIBC(atoll)
    MAP_LIBC(atof)
    MAP_LIBC(abs)
    MAP_LIBC(isspace)
    MAP_LIBC(tolower)
    MAP_LIBC(wcslen)
    MAP_LIBC(wmemchr)
    MAP_LIBC(wmemcmp)
    MAP_LIBC(ldiv)
    MAP_LIBC(difftime)
    MAP_LIBC(printf)
    MAP_LIBC(puts)
    MAP_LIBC(rand)
    MAP_LIBC(srand)
    MAP_LIBC(qsort)
    MAP_LIBC(bsearch)
    MAP_LIBC(ldexp)
    MAP_LIBC(getenv)
    MAP_LIBC(time)
    MAP_LIBC(nanosleep)
    MAP_LIBC(usleep)
    MAP_LIBC(gettimeofday)
    MAP_LIBC(abort)
    MAP_LIBC(exit)
    MAP_LIBC(_Exit)
    MAP_LIBC(sscanf)
    MAP_LIBC(vsscanf)
    MAP_LIBC(btowc)
    MAP_LIBC(wctob)
    MAP_LIBC(getcwd)
    MAP_LIBC(access)
    MAP_LIBC(remove)
    MAP_LIBC(rename)
    MAP_LIBC(rmdir)
    MAP_LIBC(unlink)
    MAP_LIBC(readlink)
    MAP_LIBC(mkdir)
    MAP_LIBC(realpath)
    MAP_LIBC(close)
    MAP_LIBC(read)
    MAP_LIBC(write)
    MAP_LIBC(fsync)
    MAP_LIBC(strerror)
    MAP_LIBC(gmtime)
    MAP_LIBC(gmtime_r)
    MAP_LIBC(strftime)
    MAP_LIBC(localtime)
    MAP_LIBC(localtime_r)
    MAP_LIBC(mktime)
    MAP_LIBC(clock)
    MAP_LIBC(tzset)
    MAP_LIBC(inet_pton)
    MAP_LIBC(inet_ntop)
    MAP_LIBC(getpid)
    if (strcmp(name, "clock_gettime") == 0)
        *address = (uint64_t)(uintptr_t)&android_clock_gettime;
    else if (strcmp(name, "memrchr") == 0)
        *address = (uint64_t)(uintptr_t)&android_memrchr;
    else if (strcmp(name, "getpagesize") == 0)
        *address = (uint64_t)(uintptr_t)&android_getpagesize;
    else if (strcmp(name, "arc4random_buf") == 0)
        *address = (uint64_t)(uintptr_t)&android_arc4random_buf;
    else if (strcmp(name, "sched_yield") == 0)
        *address = (uint64_t)(uintptr_t)&android_sched_yield;
    else if (strcmp(name, "__cmsg_nxthdr") == 0)
        *address = (uint64_t)(uintptr_t)&android_cmsg_nxthdr;
    else if (strcmp(name, "localeconv") == 0)
        *address = (uint64_t)(uintptr_t)&android_localeconv;
#undef MAP_LIBC
    return *address != 0;
}
