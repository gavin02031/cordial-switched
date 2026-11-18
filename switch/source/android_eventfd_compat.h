#ifndef ANDROID_EVENTFD_COMPAT_H
#define ANDROID_EVENTFD_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// Morrow-virtual eventfd objects. The guest's read/write/close are routed
// through android_libc_compat, which asks android_eventfd_is_virtual first;
// a virtual fd is therefore usable by the guest exactly like a kernel one,
// on host builds and on Horizon alike (no host kernel eventfd is used, so
// there is no backend divergence to audit).

// Bionic (asm-generic) flag values the guest may pass to eventfd().
#define ANDROID_EFD_SEMAPHORE 0x1
#define ANDROID_EFD_NONBLOCK  0x800
#define ANDROID_EFD_CLOEXEC   0x80000

int android_eventfd_create(unsigned initial, int flags);
bool android_eventfd_is_virtual(int fd);
ssize_t android_eventfd_read(int fd, void *buffer, size_t count);
ssize_t android_eventfd_write(int fd, const void *buffer, size_t count);
int android_eventfd_close(int fd);

#endif
