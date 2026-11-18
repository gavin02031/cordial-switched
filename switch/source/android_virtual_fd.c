#define _POSIX_C_SOURCE 200809L
#include "android_virtual_fd.h"

#include "android_eventfd_compat.h"
#include "android_timerfd_compat.h"

bool android_virtual_is_virtual(int fd)
{
    return android_eventfd_is_virtual(fd) || android_timerfd_is_virtual(fd);
}

bool android_virtual_readiness_edges(int fd, uint32_t *ready_events,
                                     uint64_t *read_edges,
                                     uint64_t *write_edges)
{
    if (android_eventfd_is_virtual(fd))
        return android_eventfd_readiness_edges(fd, ready_events,
                                               read_edges, write_edges);
    if (android_timerfd_is_virtual(fd))
        return android_timerfd_readiness_edges(fd, ready_events,
                                               read_edges, write_edges);
    return false;
}

int64_t android_virtual_next_expiry_ms(const int *fds, int count)
{
    int64_t soonest = -1;
    for (int i = 0; i < count; i++) {
        if (!android_timerfd_is_virtual(fds[i])) continue;
        const int64_t next = android_timerfd_next_expiry_ms(fds[i]);
        if (next >= 0 && (soonest < 0 || next < soonest)) soonest = next;
    }
    return soonest;
}
