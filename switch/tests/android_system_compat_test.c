#define _POSIX_C_SOURCE 200809L
#include "android_system_compat.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "android_log_compat.h"

static char captured_tag[64];
static char captured_message[1200];
static int captured_priority;
static int captured_count;

static void capture_sink(int priority, const char *tag, const char *message,
                         void *context)
{
    (void)context;
    captured_priority = priority;
    snprintf(captured_tag, sizeof(captured_tag), "%s", tag);
    snprintf(captured_message, sizeof(captured_message), "%s", message);
    captured_count++;
}

static uint64_t resolve(const char *name)
{
    uint64_t address = 0;
    assert(android_system_compat_resolve(name, "LIBC", "libc.so", &address));
    assert(address != 0);
    return address;
}

static void test_abort_message_capture(void)
{
    void (*set_abort)(const char *) =
        (void *)(uintptr_t)resolve("android_set_abort_message");
    set_abort(NULL);
    assert(strcmp(android_system_compat_abort_message(), "") == 0);
    int before = captured_count;
    set_abort("Pure virtual function called");
    assert(strcmp(android_system_compat_abort_message(),
                  "Pure virtual function called") == 0);
    assert(captured_count == before + 1);
    assert(captured_priority == 7);
    assert(strcmp(captured_tag, "abort") == 0);
    assert(strcmp(captured_message, "Pure virtual function called") == 0);
}

static void test_syslog_forwarding(void)
{
    void (*openlog_fn)(const char *, int, int) =
        (void *)(uintptr_t)resolve("openlog");
    void (*syslog_fn)(int, const char *, ...) =
        (void *)(uintptr_t)resolve("syslog");
    void (*closelog_fn)(void) = (void *)(uintptr_t)resolve("closelog");
    openlog_fn("morrow-test", 0, 1);
    int before = captured_count;
    syslog_fn(3, "player %d left", 7); // LOG_ERR
    assert(captured_count == before + 1);
    assert(captured_priority == 6);
    assert(strcmp(captured_tag, "morrow-test") == 0);
    assert(strcmp(captured_message, "player 7 left") == 0);
    syslog_fn(7, "debug line"); // LOG_DEBUG
    assert(captured_priority == 3);
    assert(strcmp(captured_message, "debug line") == 0);
    syslog_fn(0, "fatal line"); // LOG_EMERG
    assert(captured_priority == 7);
    closelog_fn();
    syslog_fn(6, "after close %s", "ident");
    assert(strcmp(captured_tag, "syslog") == 0);
    assert(strcmp(captured_message, "after close ident") == 0);
}

static void test_fail_closed_signals(void)
{
    int (*sigaction_fn)(int, const void *, void *) =
        (void *)(uintptr_t)resolve("sigaction");
    void *(*signal_fn)(int, void *) = (void *)(uintptr_t)resolve("signal");
    int (*raise_fn)(int) = (void *)(uintptr_t)resolve("raise");
    int (*sigmask_fn)(int, const void *, void *) =
        (void *)(uintptr_t)resolve("pthread_sigmask");
    int (*sigaltstack_fn)(const void *, void *) =
        (void *)(uintptr_t)resolve("sigaltstack");
    errno = 0;
    assert(sigaction_fn(11, NULL, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(signal_fn(11, NULL) == (void *)(intptr_t)-1 && errno == ENOSYS);
    errno = 0;
    assert(raise_fn(6) == -1 && errno == ENOSYS);
    assert(sigmask_fn(0, NULL, NULL) == ENOSYS);
    errno = 0;
    assert(sigaltstack_fn(NULL, NULL) == -1 && errno == ENOSYS);
}

static void test_fail_closed_scheduling(void)
{
    int (*priority_max)(int) =
        (void *)(uintptr_t)resolve("sched_get_priority_max");
    int (*priority_min)(int) =
        (void *)(uintptr_t)resolve("sched_get_priority_min");
    int (*getparam)(int32_t, void *) =
        (void *)(uintptr_t)resolve("sched_getparam");
    int (*getscheduler)(int32_t) =
        (void *)(uintptr_t)resolve("sched_getscheduler");
    int (*setscheduler)(int32_t, int, const void *) =
        (void *)(uintptr_t)resolve("sched_setscheduler");
    int (*getcpu)(void) = (void *)(uintptr_t)resolve("sched_getcpu");
    int (*getpriority_fn)(int, unsigned) =
        (void *)(uintptr_t)resolve("getpriority");
    int (*setpriority_fn)(int, unsigned, int) =
        (void *)(uintptr_t)resolve("setpriority");
    errno = 0;
    assert(priority_max(1) == -1 && errno == ENOSYS);
    errno = 0;
    assert(priority_min(1) == -1 && errno == ENOSYS);
    errno = 0;
    assert(getparam(0, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(getscheduler(0) == -1 && errno == ENOSYS);
    errno = 0;
    assert(setscheduler(0, 0, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(getcpu() == -1 && errno == ENOSYS);
    errno = 0;
    assert(getpriority_fn(0, 0) == -1 && errno == ENOSYS);
    errno = 0;
    assert(setpriority_fn(0, 0, 0) == -1 && errno == ENOSYS);
}

static void test_fail_closed_pthread_policy(void)
{
    int (*attr_setparam)(void *, const void *) =
        (void *)(uintptr_t)resolve("pthread_attr_setschedparam");
    int (*getschedparam)(uint64_t, int *, void *) =
        (void *)(uintptr_t)resolve("pthread_getschedparam");
    int (*setschedparam)(uint64_t, int, const void *) =
        (void *)(uintptr_t)resolve("pthread_setschedparam");
    int (*setname)(uint64_t, const char *) =
        (void *)(uintptr_t)resolve("pthread_setname_np");
    assert(attr_setparam(NULL, NULL) == ENOSYS);
    // pthread_getattr_np moved to the lifecycle bridge (real stack bounds).
    assert(getschedparam(0, NULL, NULL) == ENOSYS);
    assert(setschedparam(0, 0, NULL) == ENOSYS);
    assert(setname(0, "worker") == ENOSYS);
}

static void test_fail_closed_descriptors(void)
{
    int (*accept4_fn)(int, void *, void *, int) =
        (void *)(uintptr_t)resolve("accept4");
    int (*eventfd_fn)(unsigned, int) = (void *)(uintptr_t)resolve("eventfd");
    int (*timerfd_create_fn)(int, int) =
        (void *)(uintptr_t)resolve("timerfd_create");
    int (*timerfd_settime_fn)(int, int, const void *, void *) =
        (void *)(uintptr_t)resolve("timerfd_settime");
    int (*epoll_create_fn)(int) = (void *)(uintptr_t)resolve("epoll_create");
    int (*epoll_create1_fn)(int) = (void *)(uintptr_t)resolve("epoll_create1");
    int (*epoll_ctl_fn)(int, int, int, void *) =
        (void *)(uintptr_t)resolve("epoll_ctl");
    int (*epoll_wait_fn)(int, void *, int, int) =
        (void *)(uintptr_t)resolve("epoll_wait");
    int (*pipe_fn)(int *) = (void *)(uintptr_t)resolve("pipe");
    int (*socketpair_fn)(int, int, int, int *) =
        (void *)(uintptr_t)resolve("socketpair");
    int (*ioctl_fn)(int, unsigned long, ...) =
        (void *)(uintptr_t)resolve("ioctl");
    long (*syscall_fn)(long, ...) = (void *)(uintptr_t)resolve("syscall");
    errno = 0;
    assert(accept4_fn(0, NULL, NULL, 0) == -1 && errno == ENOSYS);
    errno = 0;
    assert(eventfd_fn(0, 0) == -1 && errno == ENOSYS);
    errno = 0;
    assert(timerfd_create_fn(1, 0) == -1 && errno == ENOSYS);
    errno = 0;
    assert(timerfd_settime_fn(0, 0, NULL, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(epoll_create_fn(1) == -1 && errno == ENOSYS);
    errno = 0;
    int log_before = captured_count;
    assert(epoll_create1_fn(0) == -1 && errno == ENOSYS);
    // The first refusal of a facility logs its name once.
    assert(captured_count == log_before + 1);
    assert(strstr(captured_message, "unsupported"));
    assert(strstr(captured_message, "epoll_create1"));
    assert(epoll_create1_fn(0) == -1);
    // Retries stay silent so a poll loop cannot flood the journal.
    assert(captured_count == log_before + 1);
    errno = 0;
    assert(epoll_ctl_fn(0, 0, 0, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(epoll_wait_fn(0, NULL, 0, 0) == -1 && errno == ENOSYS);
    errno = 0;
    assert(pipe_fn(NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(socketpair_fn(1, 1, 0, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(ioctl_fn(0, 0UL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(syscall_fn(0) == -1 && errno == ENOSYS);
}

static void test_fail_closed_misc(void)
{
    int (*prctl_fn)(int, ...) = (void *)(uintptr_t)resolve("prctl");
    int (*mlock_fn)(const void *, size_t) = (void *)(uintptr_t)resolve("mlock");
    int (*fallocate_fn)(int, int64_t, int64_t) =
        (void *)(uintptr_t)resolve("posix_fallocate");
    int (*statvfs_fn)(const char *, void *) =
        (void *)(uintptr_t)resolve("statvfs");
    int (*utime_fn)(const char *, const void *) =
        (void *)(uintptr_t)resolve("utime");
    int (*utimes_fn)(const char *, const void *) =
        (void *)(uintptr_t)resolve("utimes");
    int (*gethostname_fn)(char *, size_t) =
        (void *)(uintptr_t)resolve("gethostname");
    unsigned (*nametoindex)(const char *) =
        (void *)(uintptr_t)resolve("if_nametoindex");
    void *(*indextoname)(unsigned, void *) =
        (void *)(uintptr_t)resolve("if_indextoname");
    int (*tcgetattr_fn)(int, void *) = (void *)(uintptr_t)resolve("tcgetattr");
    int (*tcsetattr_fn)(int, int, const void *) =
        (void *)(uintptr_t)resolve("tcsetattr");
    errno = 0;
    assert(prctl_fn(15) == -1 && errno == ENOSYS);
    errno = 0;
    assert(mlock_fn(NULL, 0) == -1 && errno == ENOSYS);
    assert(fallocate_fn(0, 0, 0) == ENOSYS);
    errno = 0;
    assert(statvfs_fn("/", NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(utime_fn("/", NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(utimes_fn("/", NULL) == -1 && errno == ENOSYS);
    errno = 0;
    char hostname[16];
    assert(gethostname_fn(hostname, sizeof(hostname)) == -1 && errno == ENOSYS);
    errno = 0;
    assert(nametoindex("wlan0") == 0 && errno == ENODEV);
    errno = 0;
    assert(indextoname(1, hostname) == NULL && errno == ENXIO);
    errno = 0;
    assert(tcgetattr_fn(0, NULL) == -1 && errno == ENOSYS);
    errno = 0;
    assert(tcsetattr_fn(0, 0, NULL) == -1 && errno == ENOSYS);
}

static void test_properties_and_locale(void)
{
    int (*property_get)(const char *, char *) =
        (void *)(uintptr_t)resolve("__system_property_get");
    size_t (*mb_cur_max)(void) =
        (void *)(uintptr_t)resolve("__ctype_get_mb_cur_max");
    char value[64];
    memset(value, 'x', sizeof(value));
    int before = captured_count;
    // Bionic's not-found answer: empty value, zero return, no errno.
    assert(property_get("ro.build.version.sdk", value) == 0);
    assert(value[0] == '\0');
    assert(captured_count == before + 1);
    assert(captured_priority == 4);
    assert(strcmp(captured_tag, "properties") == 0);
    assert(strstr(captured_message, "ro.build.version.sdk"));
    // A repeated name is served without another log line.
    assert(property_get("ro.build.version.sdk", value) == 0);
    assert(captured_count == before + 1);
    assert(property_get("ro.product.model", value) == 0);
    assert(captured_count == before + 2);
    assert(property_get(NULL, value) == 0);
    assert(property_get("ro.serialno", NULL) == 0);
    // Bionic's C.UTF-8 default: four-byte multibyte maximum.
    assert(mb_cur_max() == 4);
}

static void test_refusal_logging(void)
{
    // Every provider was already exercised by the fail-closed tests above,
    // so repeat calls now must refuse without another log line.
    int (*pthread_setname_fn)(uint64_t, const char *) =
        (void *)(uintptr_t)resolve("pthread_setname_np");
    int before = captured_count;
    assert(pthread_setname_fn(0, "x") != 0);
    assert(captured_count == before);
}

static void test_sysinfo_and_uname(void)
{
    int (*sysinfo_fn)(void *) = (void *)(uintptr_t)resolve("sysinfo");
    int (*uname_fn)(void *) = (void *)(uintptr_t)resolve("uname");
    char info[112];
    memset(info, 0, sizeof(info));
    assert(sysinfo_fn(info) == 0);
    // The 112-byte kernel layout is asserted in the provider; here the
    // host figures must look like a live machine's.
    unsigned long long totalram, freeram;
    memcpy(&totalram, info + 32, 8);
    memcpy(&freeram, info + 40, 8);
    assert(totalram > 0);
    assert(freeram <= totalram);
    unsigned mem_unit;
    memcpy(&mem_unit, info + 104, 4);
    assert(mem_unit == 1);
    errno = 0;
    assert(sysinfo_fn(NULL) == -1 && errno == EFAULT);
    char uts[390];
    memset(uts, 0, sizeof(uts));
    assert(uname_fn(uts) == 0);
    assert(uts[0] != '\0');       // sysname answered
    assert(uts[260] != '\0');     // machine answered
    assert(uname_fn(NULL) == -1 && errno == EFAULT);
}

int main(void)
{
    android_log_compat_set_sink(capture_sink, NULL);
    test_abort_message_capture();
    test_syslog_forwarding();
    test_fail_closed_signals();
    test_fail_closed_scheduling();
    test_fail_closed_pthread_policy();
    test_fail_closed_descriptors();
    test_fail_closed_misc();
    test_properties_and_locale();
    test_sysinfo_and_uname();
    test_refusal_logging();
    const char *names[] = {
        "android_set_abort_message", "__system_property_get",
        "__ctype_get_mb_cur_max", "sysinfo", "uname", "openlog",
        "syslog", "closelog",
        "sigaction", "signal", "raise", "pthread_sigmask", "sigaltstack",
        "sched_get_priority_max", "sched_get_priority_min", "sched_getparam",
        "sched_getscheduler", "sched_setscheduler", "sched_getcpu",
        "getpriority", "setpriority", "pthread_attr_setschedparam",
        "pthread_getschedparam", "pthread_setschedparam",
        "pthread_setname_np", "accept4", "eventfd", "timerfd_create",
        "timerfd_settime", "epoll_create", "epoll_create1", "epoll_ctl",
        "epoll_wait", "pipe", "socketpair", "ioctl", "syscall", "prctl",
        "mlock", "posix_fallocate", "statvfs", "utime", "utimes",
        "gethostname", "if_nametoindex", "if_indextoname", "tcgetattr",
        "tcsetattr",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint64_t address = 0x1234;
        assert(!android_system_compat_resolve(names[i], "LIBC_N", "libc.so", &address));
        assert(!android_system_compat_resolve(names[i], "LIBC", "libdl.so", &address));
        assert(!android_system_compat_resolve(names[i], NULL, NULL, &address));
    }
    uint64_t address = 0;
    assert(!android_system_compat_resolve("mallinfo", "LIBC", "libc.so", &address));
    assert(!address);
    return 0;
}
