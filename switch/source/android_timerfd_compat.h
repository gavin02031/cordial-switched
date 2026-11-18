#ifndef ANDROID_TIMERFD_COMPAT_H
#define ANDROID_TIMERFD_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Morrow-virtual timerfd objects (CLOCK_MONOTONIC and CLOCK_REALTIME).
// Same code on host and Horizon; expirations are computed lazily from the
// clock, so no helper thread exists to wedge or drift.

// Bionic (asm-generic) flag values the guest may pass.
#define ANDROID_TFD_NONBLOCK 0x800
#define ANDROID_TFD_CLOEXEC  0x80000
#define ANDROID_TFD_TIMER_ABSTIME 0x1

// Guest struct itimerspec (aarch64 LP64: two { i64 sec, i64 nsec }).
typedef struct {
    int64_t interval_sec;
    int64_t interval_nsec;
    int64_t value_sec;
    int64_t value_nsec;
} AndroidItimerspec;
_Static_assert(sizeof(AndroidItimerspec) == 32, "bionic aarch64 itimerspec");

int android_timerfd_create(int clockid, int flags);
bool android_timerfd_is_virtual(int fd);
int android_timerfd_settime(int fd, int flags,
                            const AndroidItimerspec *next,
                            AndroidItimerspec *old);
ssize_t android_timerfd_read(int fd, void *buffer, size_t count);
int android_timerfd_close(int fd);

// epoll support, same contract as the eventfd edges interface:
// read_edges counts transitions of the unread expiration count from
// zero to positive (the Linux EPOLLET edge source for timerfd), not
// the raw number of firings.
bool android_timerfd_readiness_edges(int fd, uint32_t *ready_events,
                                     uint64_t *read_edges,
                                     uint64_t *write_edges);
// Milliseconds until the next expiry, rounded up so a waiter never
// wakes early; 0 if due now, -1 if nothing armed.
int64_t android_timerfd_next_expiry_ms(int fd);

#endif
