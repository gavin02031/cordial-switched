#ifndef ANDROID_VIRTUAL_FD_H
#define ANDROID_VIRTUAL_FD_H

#include <stdbool.h>
#include <stdint.h>

// Dispatch over every Morrow-virtual descriptor kind (eventfd, timerfd).
// Consumers (looper, epoll) use this instead of per-kind checks so a new
// descriptor kind cannot silently become never-ready in one of them.

bool android_virtual_is_virtual(int fd);
bool android_virtual_readiness_edges(int fd, uint32_t *ready_events,
                                     uint64_t *read_edges,
                                     uint64_t *write_edges);
// Milliseconds until the next time-driven expiry among the given fds,
// 0 if one is due, -1 if none is pending. fds is a watch list snapshot.
int64_t android_virtual_next_expiry_ms(const int *fds, int count);

// Linux removes epoll watches when the watched file description is
// released. Virtual-fd close paths call android_virtual_notify_close so
// interested consumers (epoll) can drop watches at close time, not at
// some later scan. The hook is registered once by the epoll module;
// it must be idempotent and must not call back into the closing object.
void android_virtual_register_close_hook(void (*hook)(int fd));
void android_virtual_notify_close(int fd);

#endif
