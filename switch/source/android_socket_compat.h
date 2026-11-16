#ifndef MORROW_ANDROID_SOCKET_COMPAT_H
#define MORROW_ANDROID_SOCKET_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Bionic's Linux sockaddr family fields are not the BSD sockaddr structures
// used by libnx. Only verified IPv4/IPv6 translations can be exposed.
bool android_socket_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address);

#endif
