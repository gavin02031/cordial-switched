#define _POSIX_C_SOURCE 200809L
#include "android_socket_compat.h"

#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <stdatomic.h>
#include <switch.h>

static atomic_int network_state;

static bool ready(void)
{
    if (atomic_load(&network_state) == 2) return true;
    int expected = 0;
    if (atomic_compare_exchange_strong(&network_state, &expected, 1)) {
        // Never initialise BSD services during the read-only import audit.
        // The first actual guest socket request is the explicit boundary.
        const Result result = socketInitializeDefault();
        atomic_store(&network_state, R_SUCCEEDED(result) ? 2 : 3);
    } else {
        while (atomic_load(&network_state) == 1)
            svcSleepThread(1000000ULL);
    }
    if (atomic_load(&network_state) == 2) return true;
    errno = ENETDOWN;
    return false;
}
#else
static bool ready(void) { return true; }
#endif

enum {
    GUEST_AF_INET = 2,
    GUEST_AF_INET6 = 10,
    GUEST_SOCK_STREAM = 1,
    GUEST_SOCK_DGRAM = 2,
    GUEST_SOCK_RAW = 3,
    GUEST_SOCK_NONBLOCK = 00004000,
    GUEST_SOCK_CLOEXEC = 02000000,
    GUEST_MSG_OOB = 1,
    GUEST_MSG_PEEK = 2,
    GUEST_MSG_DONTWAIT = 0x40,
    GUEST_MSG_WAITALL = 0x100,
    GUEST_MSG_NOSIGNAL = 0x4000,
};

_Static_assert(sizeof(struct sockaddr_in) == 16, "Native IPv4 sockaddr must be 16 bytes");
_Static_assert(sizeof(struct sockaddr_in6) == 28, "Native IPv6 sockaddr must be 28 bytes");
#ifdef __SWITCH__
// libnx exposes two native layouts: IPv4 has a leading length byte, while
// IPv6 starts directly with a 16-bit family (no sin6_len field).
_Static_assert(offsetof(struct sockaddr_in, sin_family) == 1, "libnx IPv4 family offset");
_Static_assert(offsetof(struct sockaddr_in6, sin6_family) == 0, "libnx IPv6 family offset");
#endif

static bool translate_domain(int guest, int *native)
{
    if (guest == GUEST_AF_INET) *native = AF_INET;
    else if (guest == GUEST_AF_INET6) *native = AF_INET6;
    else { errno = EAFNOSUPPORT; return false; }
    return true;
}

// Bionic uses a 16-bit family first. libnx IPv4 uses length+8-bit family,
// but libnx IPv6 uses a 16-bit family first (with a different AF_INET6 value).
// Ports and IPv4/v6 address bytes are network-order.
static bool to_native(const void *guest, uint32_t guest_len,
                      struct sockaddr_storage *native, socklen_t *native_len)
{
    if (!guest || guest_len < 2) { errno = EINVAL; return false; }
    uint16_t family;
    memcpy(&family, guest, sizeof(family));
    memset(native, 0, sizeof(*native));
    const unsigned char *data = guest;
    if (family == GUEST_AF_INET && guest_len >= 16) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)native;
        v4->sin_family = AF_INET;
#ifdef __SWITCH__
        v4->sin_len = 16;
#endif
        memcpy(&v4->sin_port, data + 2, 2);
        memcpy(&v4->sin_addr, data + 4, 4);
        *native_len = sizeof(*v4);
        return true;
    }
    if (family == GUEST_AF_INET6 && guest_len >= 28) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)native;
        v6->sin6_family = AF_INET6;
        memcpy(&v6->sin6_port, data + 2, 2);
        memcpy(&v6->sin6_flowinfo, data + 4, 4);
        memcpy(&v6->sin6_addr, data + 8, 16);
        memcpy(&v6->sin6_scope_id, data + 24, 4);
        *native_len = sizeof(*v6);
        return true;
    }
    errno = family == GUEST_AF_INET || family == GUEST_AF_INET6
        ? EINVAL : EAFNOSUPPORT;
    return false;
}

static bool to_guest(const struct sockaddr_storage *native, void *guest,
                     uint32_t *guest_len)
{
    if (!guest || !guest_len) { errno = EFAULT; return false; }
    unsigned char output[28] = {0};
    uint32_t required;
    // Never read sockaddr_storage.ss_family to dispatch both formats: on
    // Switch its BSD byte offset does not describe sockaddr_in6.sin6_family.
    if (((const struct sockaddr_in *)native)->sin_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)native;
        uint16_t family = GUEST_AF_INET;
        memcpy(output, &family, 2);
        memcpy(output + 2, &v4->sin_port, 2);
        memcpy(output + 4, &v4->sin_addr, 4);
        required = 16;
    } else if (((const struct sockaddr_in6 *)native)->sin6_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)native;
        uint16_t family = GUEST_AF_INET6;
        memcpy(output, &family, 2);
        memcpy(output + 2, &v6->sin6_port, 2);
        memcpy(output + 4, &v6->sin6_flowinfo, 4);
        memcpy(output + 8, &v6->sin6_addr, 16);
        memcpy(output + 24, &v6->sin6_scope_id, 4);
        required = 28;
    } else {
        errno = EAFNOSUPPORT;
        return false;
    }
    const size_t count = *guest_len < required ? *guest_len : required;
    memcpy(guest, output, count);
    *guest_len = required;
    return true;
}

static bool translate_message_flags(int guest, int *native)
{
    *native = 0;
#define MAP_FLAG(guest_flag, native_flag) \
    if ((guest & guest_flag) != 0) { *native |= native_flag; guest &= ~guest_flag; }
    MAP_FLAG(GUEST_MSG_OOB, MSG_OOB)
    MAP_FLAG(GUEST_MSG_PEEK, MSG_PEEK)
#ifdef MSG_DONTWAIT
    MAP_FLAG(GUEST_MSG_DONTWAIT, MSG_DONTWAIT)
#endif
#ifdef MSG_WAITALL
    MAP_FLAG(GUEST_MSG_WAITALL, MSG_WAITALL)
#endif
    // Horizon does not deliver POSIX SIGPIPE to this NRO; suppressing a
    // SIGPIPE is therefore a no-op, while EPIPE still comes back normally.
    guest &= ~GUEST_MSG_NOSIGNAL;
#undef MAP_FLAG
    if (guest) { errno = EOPNOTSUPP; return false; }
    return true;
}

static int guest_socket(int domain, int type, int protocol)
{
    int native_domain;
    if (!translate_domain(domain, &native_domain)) return -1;
    // The flag bits are Linux-specific and must not be misread as BSD types.
    if (type & (GUEST_SOCK_NONBLOCK | GUEST_SOCK_CLOEXEC)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (type != GUEST_SOCK_STREAM && type != GUEST_SOCK_DGRAM &&
        type != GUEST_SOCK_RAW) { errno = EPROTOTYPE; return -1; }
    if (!ready()) return -1;
    return socket(native_domain, type, protocol);
}

static int guest_connect(int fd, const void *address, uint32_t address_len)
{
    struct sockaddr_storage native;
    socklen_t length;
    if (!to_native(address, address_len, &native, &length) || !ready()) return -1;
    return connect(fd, (const struct sockaddr *)&native, length);
}

static int guest_bind(int fd, const void *address, uint32_t address_len)
{
    struct sockaddr_storage native;
    socklen_t length;
    if (!to_native(address, address_len, &native, &length) || !ready()) return -1;
    return bind(fd, (const struct sockaddr *)&native, length);
}

static int guest_listen(int fd, int backlog)
{
    if (!ready()) return -1;
    return listen(fd, backlog);
}

static int guest_shutdown(int fd, int how)
{
    if (how < 0 || how > 2) { errno = EINVAL; return -1; }
    if (!ready()) return -1;
    return shutdown(fd, how);
}

static int guest_accept(int fd, void *address, uint32_t *address_len)
{
    if (!!address != !!address_len) { errno = EFAULT; return -1; }
    if (!ready()) return -1;
    if (!address) return accept(fd, NULL, NULL);
    struct sockaddr_storage native = {0};
    socklen_t length = sizeof(native);
    int accepted = accept(fd, (struct sockaddr *)&native, &length);
    if (accepted < 0) return -1;
    if (!to_guest(&native, address, address_len)) {
        close(accepted);
        return -1;
    }
    return accepted;
}

static int guest_getsockname(int fd, void *address, uint32_t *address_len)
{
    if (!address || !address_len) { errno = EFAULT; return -1; }
    if (!ready()) return -1;
    struct sockaddr_storage native = {0};
    socklen_t length = sizeof(native);
    if (getsockname(fd, (struct sockaddr *)&native, &length) < 0) return -1;
    return to_guest(&native, address, address_len) ? 0 : -1;
}

static int guest_getpeername(int fd, void *address, uint32_t *address_len)
{
    if (!address || !address_len) { errno = EFAULT; return -1; }
    if (!ready()) return -1;
    struct sockaddr_storage native = {0};
    socklen_t length = sizeof(native);
    if (getpeername(fd, (struct sockaddr *)&native, &length) < 0) return -1;
    return to_guest(&native, address, address_len) ? 0 : -1;
}

static ssize_t guest_sendto(int fd, const void *buffer, size_t size, int flags,
                            const void *address, uint32_t address_len)
{
    int native_flags;
    if (!translate_message_flags(flags, &native_flags)) return -1;
    struct sockaddr_storage native;
    socklen_t length = 0;
    if (address && !to_native(address, address_len, &native, &length)) return -1;
    if (!address && address_len) { errno = EINVAL; return -1; }
    if (!ready()) return -1;
    return sendto(fd, buffer, size, native_flags,
                  address ? (const struct sockaddr *)&native : NULL, length);
}

static ssize_t guest_recvfrom(int fd, void *buffer, size_t size, int flags,
                              void *address, uint32_t *address_len)
{
    int native_flags;
    if (!translate_message_flags(flags, &native_flags)) return -1;
    if (!!address != !!address_len) { errno = EFAULT; return -1; }
    if (!ready()) return -1;
    if (!address) return recvfrom(fd, buffer, size, native_flags, NULL, NULL);
    struct sockaddr_storage native = {0};
    socklen_t length = sizeof(native);
    ssize_t result = recvfrom(fd, buffer, size, native_flags,
                              (struct sockaddr *)&native, &length);
    if (result < 0) return -1;
    return to_guest(&native, address, address_len) ? result : -1;
}

bool android_socket_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so"))
        return false;
    *address = 0;
#define MAP(symbol, target) if (!strcmp(name, symbol)) *address = (uint64_t)(uintptr_t)&target; else
    MAP("socket", guest_socket)
    MAP("connect", guest_connect)
    MAP("bind", guest_bind)
    MAP("listen", guest_listen)
    MAP("shutdown", guest_shutdown)
    MAP("accept", guest_accept)
    MAP("getsockname", guest_getsockname)
    MAP("getpeername", guest_getpeername)
    MAP("sendto", guest_sendto)
    MAP("recvfrom", guest_recvfrom)
    { return false; }
#undef MAP
    return true;
}
