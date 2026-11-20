#ifndef ANDROID_EPOLL_COMPAT_H
#define ANDROID_EPOLL_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Morrow-virtual epoll instances over Morrow-virtual descriptors. Same
// code on host and Horizon; no host kernel epoll is used.

int android_epoll_create(void);
bool android_epoll_is_virtual(int fd);
int android_epoll_ctl(int epfd, int operation, int fd,
                      uint32_t events, uint64_t data);
// Fills caller buffers with (events, data) pairs, up to maxevents.
int android_epoll_wait(int epfd, uint32_t *out_events, uint64_t *out_data,
                       int maxevents, int timeout_millis);
int android_epoll_close(int fd);

#endif
