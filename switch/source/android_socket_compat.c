#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "android_socket_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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
    GUEST_AI_PASSIVE = 0x0001,
    GUEST_AI_CANONNAME = 0x0002,
    GUEST_AI_NUMERICHOST = 0x0004,
    GUEST_AI_NUMERICSERV = 0x0008,
    GUEST_AI_ALL = 0x0100,
    GUEST_AI_ADDRCONFIG = 0x0400,
    GUEST_AI_V4MAPPED = 0x0800,
};

// Bionic puts ai_canonname before ai_addr. Linux/glibc puts ai_addr first;
// copying native struct addrinfo into guest storage would corrupt pointers.
typedef struct GuestAddrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    uint32_t ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct GuestAddrinfo *ai_next;
} GuestAddrinfo;

_Static_assert(sizeof(GuestAddrinfo) == 48, "Android LP64 addrinfo size");
_Static_assert(offsetof(GuestAddrinfo, ai_canonname) == 24, "Bionic canonname slot");
_Static_assert(offsetof(GuestAddrinfo, ai_addr) == 32, "Bionic address slot");
_Static_assert(offsetof(GuestAddrinfo, ai_next) == 40, "Bionic next slot");

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
#ifdef MSG_NOSIGNAL
    MAP_FLAG(GUEST_MSG_NOSIGNAL, MSG_NOSIGNAL)
#else
    // Horizon does not deliver POSIX SIGPIPE to this NRO; suppressing a
    // SIGPIPE is therefore a no-op, while EPIPE still comes back normally.
    guest &= ~GUEST_MSG_NOSIGNAL;
#endif
#undef MAP_FLAG
    if (guest) { errno = EOPNOTSUPP; return false; }
    return true;
}

static int guest_socket(int domain, int type, int protocol)
{
    int native_domain;
    if (!translate_domain(domain, &native_domain)) return -1;
    // Android's socket type flags cannot be passed straight to Horizon BSD.
    // libnx's BSD header defines these as 0x20000000 and 0x10000000,
    // respectively; neither has the Linux/Bionic bit assignment.
    const int flags = type & (GUEST_SOCK_NONBLOCK | GUEST_SOCK_CLOEXEC);
    const int base = type & ~(GUEST_SOCK_NONBLOCK | GUEST_SOCK_CLOEXEC);
    if (base != GUEST_SOCK_STREAM && base != GUEST_SOCK_DGRAM &&
        base != GUEST_SOCK_RAW) { errno = EPROTOTYPE; return -1; }
    int native_type = base;
#ifdef __SWITCH__
    if (flags & GUEST_SOCK_NONBLOCK) native_type |= 0x20000000;
    if (flags & GUEST_SOCK_CLOEXEC) native_type |= 0x10000000;
#elif defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    if (flags & GUEST_SOCK_NONBLOCK) native_type |= SOCK_NONBLOCK;
    if (flags & GUEST_SOCK_CLOEXEC) native_type |= SOCK_CLOEXEC;
#else
    if (flags) { errno = EOPNOTSUPP; return -1; }
#endif
    if (!ready()) return -1;
    return socket(native_domain, native_type, protocol);
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

static ssize_t guest_send(int fd, const void *buffer, size_t size, int flags)
{
    return guest_sendto(fd, buffer, size, flags, NULL, 0);
}

static ssize_t guest_recv(int fd, void *buffer, size_t size, int flags)
{
    return guest_recvfrom(fd, buffer, size, flags, NULL, NULL);
}

// Android SOL_SOCKET (1) and its option numbers do not match libnx's BSD
// SOL_SOCKET (0xffff). Restrict to int-valued options whose semantics match.
static bool native_socket_option(int guest_level, int guest_option,
                                 int *native_level, int *native_option)
{
    if (guest_level != 1) { errno = ENOPROTOOPT; return false; }
    *native_level = SOL_SOCKET;
    switch (guest_option) {
    case 2: *native_option = SO_REUSEADDR; return true;
    case 6: *native_option = SO_BROADCAST; return true;
    case 7: *native_option = SO_SNDBUF; return true;
    case 8: *native_option = SO_RCVBUF; return true;
    case 9: *native_option = SO_KEEPALIVE; return true;
    default: errno = ENOPROTOOPT; return false;
    }
}

static int guest_setsockopt(int fd, int level, int option,
                            const void *value, uint32_t value_len)
{
    int native_level, native_option;
    if (!native_socket_option(level, option, &native_level, &native_option)) return -1;
    if (!value) { errno = EFAULT; return -1; }
    if (value_len != sizeof(int)) { errno = EINVAL; return -1; }
    if (!ready()) return -1;
    return setsockopt(fd, native_level, native_option, value, sizeof(int));
}

static int guest_getsockopt(int fd, int level, int option,
                            void *value, uint32_t *value_len)
{
    int native_level, native_option;
    if (!native_socket_option(level, option, &native_level, &native_option)) return -1;
    if (!value || !value_len) { errno = EFAULT; return -1; }
    if (*value_len < sizeof(int)) { errno = EINVAL; return -1; }
    if (!ready()) return -1;
    socklen_t native_len = sizeof(int);
    const int status = getsockopt(fd, native_level, native_option, value, &native_len);
    if (!status) *value_len = native_len;
    return status;
}

static int guest_inet_pton(int family, const char *text, void *address)
{
    int native_family;
    if (!translate_domain(family, &native_family)) return -1;
    if (!text || !address) { errno = EFAULT; return -1; }
    return inet_pton(native_family, text, address);
}

static const char *guest_inet_ntop(int family, const void *address, char *text,
                                   uint32_t text_len)
{
    int native_family;
    if (!translate_domain(family, &native_family)) return NULL;
    if (!address || !text) { errno = EFAULT; return NULL; }
    return inet_ntop(native_family, address, text, (socklen_t)text_len);
}

// Android's EAI_* values are positive, whereas native providers may use
// different constants (glibc uses negative values). Keep native DNS errors
// out of the Android result domain.
static int guest_eai(int native)
{
    if (native == 0) return 0;
    if (native == EAI_AGAIN) return 2;
    if (native == EAI_BADFLAGS) return 3;
    if (native == EAI_FAIL) return 4;
    if (native == EAI_FAMILY) return 5;
    if (native == EAI_MEMORY) return 6;
#ifdef EAI_NODATA
    if (native == EAI_NODATA) return 7;
#endif
    if (native == EAI_NONAME) return 8;
    if (native == EAI_SERVICE) return 9;
    if (native == EAI_SOCKTYPE) return 10;
    if (native == EAI_SYSTEM) return 11;
#ifdef EAI_BADHINTS
    if (native == EAI_BADHINTS) return 12;
#endif
#ifdef EAI_PROTOCOL
    if (native == EAI_PROTOCOL) return 13;
#endif
#ifdef EAI_OVERFLOW
    if (native == EAI_OVERFLOW) return 14;
#endif
    return 4;
}

// Bionic declares size_t lengths here, unlike POSIX's socklen_t. Reject
// truncation before calling the native resolver with a smaller capacity.
static int guest_getnameinfo(const void *address, uint32_t length,
                              char *host, size_t host_size,
                              char *service, size_t service_size, int flags)
{
    int native_flags = 0;
#define NI_MAP(g, n) if (flags & (g)) { native_flags |= (n); flags &= ~(g); }
    NI_MAP(1, NI_NOFQDN)
    NI_MAP(2, NI_NUMERICHOST)
    NI_MAP(4, NI_NAMEREQD)
    NI_MAP(8, NI_NUMERICSERV)
    NI_MAP(16, NI_DGRAM)
#undef NI_MAP
    if (flags) return 3;
    if ((size_t)(socklen_t)host_size != host_size ||
        (size_t)(socklen_t)service_size != service_size) return 14;
    if ((!host && host_size) || (!service && service_size)) {
        errno = EFAULT; return 11;
    }
    struct sockaddr_storage native;
    socklen_t native_length;
    if (!to_native(address, length, &native, &native_length)) return 5;
    if (!ready()) return 11;
    return guest_eai(getnameinfo((const struct sockaddr *)&native, native_length,
                                host, (socklen_t)host_size,
                                service, (socklen_t)service_size, native_flags));
}

static bool native_socktype(int guest, int *native)
{
    switch (guest) {
    case 0: *native = 0; return true;
    case GUEST_SOCK_STREAM: *native = SOCK_STREAM; return true;
    case GUEST_SOCK_DGRAM: *native = SOCK_DGRAM; return true;
    case GUEST_SOCK_RAW: *native = SOCK_RAW; return true;
    default: return false;
    }
}

static bool guest_socktype(int native, int *guest)
{
    if (native == 0) *guest = 0;
    else if (native == SOCK_STREAM) *guest = GUEST_SOCK_STREAM;
    else if (native == SOCK_DGRAM) *guest = GUEST_SOCK_DGRAM;
    else if (native == SOCK_RAW) *guest = GUEST_SOCK_RAW;
    else return false;
    return true;
}

static bool native_ai_flags(int guest, int *native)
{
    *native = 0;
#define AI_MAP(guest_flag, native_flag) \
    if (guest & guest_flag) { *native |= native_flag; guest &= ~guest_flag; }
    AI_MAP(GUEST_AI_PASSIVE, AI_PASSIVE)
    AI_MAP(GUEST_AI_CANONNAME, AI_CANONNAME)
    AI_MAP(GUEST_AI_NUMERICHOST, AI_NUMERICHOST)
#ifdef AI_NUMERICSERV
    AI_MAP(GUEST_AI_NUMERICSERV, AI_NUMERICSERV)
#endif
#ifdef AI_ALL
    AI_MAP(GUEST_AI_ALL, AI_ALL)
#endif
#ifdef AI_ADDRCONFIG
    AI_MAP(GUEST_AI_ADDRCONFIG, AI_ADDRCONFIG)
#endif
#ifdef AI_V4MAPPED
    AI_MAP(GUEST_AI_V4MAPPED, AI_V4MAPPED)
#endif
#undef AI_MAP
    return guest == 0;
}

static void guest_freeaddrinfo(GuestAddrinfo *entry)
{
    while (entry) {
        GuestAddrinfo *next = entry->ai_next;
        free(entry->ai_canonname);
        free(entry->ai_addr);
        free(entry);
        entry = next;
    }
}

static int guest_getaddrinfo(const char *host, const char *service,
                             const GuestAddrinfo *hints, GuestAddrinfo **result)
{
    if (!result) { errno = EFAULT; return 11; }
    *result = NULL;
    if (!host && !service) return 8;

    struct addrinfo native_hints = {0};
    const struct addrinfo *native_hints_ptr = NULL;
    if (hints) {
        if (hints->ai_addr || hints->ai_canonname || hints->ai_next || hints->ai_addrlen)
            return 12; // EAI_BADHINTS: input hints are not a result list
        if (hints->ai_family != 0 && !translate_domain(hints->ai_family,
                                                       &native_hints.ai_family)) return 5;
        if (!native_socktype(hints->ai_socktype, &native_hints.ai_socktype)) return 10;
        if (hints->ai_protocol != 0 && hints->ai_protocol != IPPROTO_TCP &&
            hints->ai_protocol != IPPROTO_UDP) return 13;
        native_hints.ai_protocol = hints->ai_protocol;
        if (!native_ai_flags(hints->ai_flags, &native_hints.ai_flags)) return 3;
        native_hints_ptr = &native_hints;
    }
    if (!ready()) return 11;

    struct addrinfo *native_results = NULL;
    const int error = getaddrinfo(host, service, native_hints_ptr, &native_results);
    if (error) return guest_eai(error);

    GuestAddrinfo *first = NULL;
    GuestAddrinfo **tail = &first;
    int outcome = 0;
    unsigned count = 0;
    for (const struct addrinfo *current = native_results; current; current = current->ai_next) {
        if (++count > 128) { outcome = 6; break; }
        uint32_t guest_length;
        int guest_family;
        if (current->ai_family == AF_INET && current->ai_addrlen >= 16) {
            guest_family = GUEST_AF_INET;
            guest_length = 16;
        } else if (current->ai_family == AF_INET6 && current->ai_addrlen >= 28) {
            guest_family = GUEST_AF_INET6;
            guest_length = 28;
        } else { outcome = 5; break; }
        if (!current->ai_addr ||
            current->ai_addrlen > sizeof(struct sockaddr_storage)) {
            outcome = 4;
            break;
        }
        int guest_type;
        if (!guest_socktype(current->ai_socktype, &guest_type)) {
            outcome = 10;
            break;
        }
        GuestAddrinfo *entry = calloc(1, sizeof(*entry));
        if (!entry) { outcome = 6; break; }
        entry->ai_addr = calloc(1, guest_length);
        if (current->ai_canonname) entry->ai_canonname = strdup(current->ai_canonname);
        if (!entry->ai_addr || (current->ai_canonname && !entry->ai_canonname)) {
            free(entry->ai_addr);
            free(entry->ai_canonname);
            free(entry);
            outcome = 6;
            break;
        }
        struct sockaddr_storage native = {0};
        memcpy(&native, current->ai_addr, current->ai_addrlen);
        uint32_t out_length = guest_length;
        if (!to_guest(&native, entry->ai_addr, &out_length)) {
            free(entry->ai_addr);
            free(entry->ai_canonname);
            free(entry);
            outcome = 5;
            break;
        }
        entry->ai_addrlen = out_length;
        entry->ai_family = guest_family;
        entry->ai_socktype = guest_type;
        entry->ai_protocol = current->ai_protocol;
        // Return only canonical-name and address information we can validate.
        entry->ai_flags = hints ? hints->ai_flags : 0;
        *tail = entry;
        tail = &entry->ai_next;
    }
    freeaddrinfo(native_results);
    if (outcome || !first) {
        guest_freeaddrinfo(first);
        return outcome ? outcome : 4;
    }
    *result = first;
    return 0;
}

static const char *guest_gai_strerror(int error)
{
    static const char *const messages[] = {
        "Success", "Address family not supported for hostname",
        "Temporary failure in name resolution", "Invalid address flags",
        "Name resolution failed", "Address family not supported",
        "Out of memory", "No address associated with hostname",
        "Name or service not known", "Service not supported",
        "Socket type not supported", "System error", "Invalid address hints",
        "Unknown protocol", "Address buffer overflow"
    };
    return (error >= 0 && (unsigned)error < sizeof(messages) / sizeof(messages[0]))
        ? messages[error] : "Unknown address resolution error";
}

// Bionic poll.h uses a 32-bit count and Linux event bits. Translate even
// where today's host happens to use the same numbers (libnx uses BSD).
typedef struct { int32_t fd; int16_t events, revents; } GuestPollFd;
_Static_assert(sizeof(GuestPollFd) == 8, "Bionic pollfd stride");

static bool poll_to_native(short events, short *native)
{
    short out = 0;
    unsigned accepted = 0x038;
#define EVENT(g, n) accepted |= (g); if (events & (g)) out |= (n);
    EVENT(0x001, POLLIN)
    EVENT(0x002, POLLPRI)
    EVENT(0x004, POLLOUT)
#ifdef POLLRDNORM
    EVENT(0x040, POLLRDNORM)
#endif
#ifdef POLLRDBAND
    EVENT(0x080, POLLRDBAND)
#endif
#ifdef POLLWRNORM
    EVENT(0x100, POLLWRNORM)
#endif
#ifdef POLLWRBAND
    EVENT(0x200, POLLWRBAND)
#endif
#undef EVENT
    if ((unsigned short)events & ~accepted) { errno = ENOTSUP; return false; }
    *native = out;
    return true;
}

static short poll_to_guest(short events)
{
    short out = 0;
#define EVENT(g, n) if (events & (n)) out |= (g);
    EVENT(0x001, POLLIN)
    EVENT(0x002, POLLPRI)
    EVENT(0x004, POLLOUT)
    EVENT(0x008, POLLERR)
    EVENT(0x010, POLLHUP)
    EVENT(0x020, POLLNVAL)
#ifdef POLLRDNORM
    EVENT(0x040, POLLRDNORM)
#endif
#ifdef POLLRDBAND
    EVENT(0x080, POLLRDBAND)
#endif
#ifdef POLLWRNORM
    EVENT(0x100, POLLWRNORM)
#endif
#ifdef POLLWRBAND
    EVENT(0x200, POLLWRBAND)
#endif
#undef EVENT
    return out;
}

static int guest_poll(GuestPollFd *fds, uint32_t count, int timeout)
{
    // Bound temporary allocations before examining guest memory.
    if (count > 65536) { errno = EINVAL; return -1; }
    if (count && !fds) { errno = EFAULT; return -1; }
    struct pollfd local[32];
    struct pollfd *native = count <= 32 ? local : calloc(count, sizeof(*native));
    if (!native) { errno = ENOMEM; return -1; }
    for (uint32_t i = 0; i < count; ++i) {
        native[i].fd = fds[i].fd;
        native[i].events = 0;
        if (fds[i].fd >= 0 && !poll_to_native(fds[i].events, &native[i].events)) {
            if (native != local) free(native);
            return -1;
        }
        native[i].revents = 0;
    }
    int result = ready() ? poll(native, count, timeout) : -1;
    int saved = errno;
    if (result >= 0) {
        for (uint32_t i = 0; i < count; ++i) {
            // BSD may alias normal-write to POLLOUT. Do not report readiness
            // bits the guest did not request, apart from ERR/HUP/NVAL.
            fds[i].revents = poll_to_guest(native[i].revents) &
                (fds[i].events | 0x038);
        }
    }
    if (native != local) free(native);
    errno = saved;
    return result;
}

// Bionic LP64 fd_set is 16 uint64_t words; native FD_SETSIZE and mask word
// width need not match it. Never pass guest sets directly into libc.
static bool set_contains(const void *set, int fd)
{
    uint64_t word;
    memcpy(&word, (const unsigned char *)set + (fd / 64) * 8, 8);
    return (word & (UINT64_C(1) << (fd % 64))) != 0;
}

static int guest_select(int count, void *reads, void *writes, void *exceptions,
                         int64_t *timeout)
{
    if (count < 0 || count > 1024 || count > FD_SETSIZE) {
        errno = EINVAL; return -1;
    }
    struct timeval native_time, *time_pointer = NULL;
    if (timeout) {
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1000000) {
            errno = EINVAL; return -1;
        }
        native_time.tv_sec = timeout[0];
        native_time.tv_usec = timeout[1];
        if ((int64_t)native_time.tv_sec != timeout[0]) { errno = EOVERFLOW; return -1; }
        time_pointer = &native_time;
    }
    void *guest[3] = {reads, writes, exceptions};
    fd_set native[3];
    for (int n = 0; n < 3; ++n) {
        FD_ZERO(&native[n]);
        if (guest[n]) for (int fd = 0; fd < count; ++fd)
            if (set_contains(guest[n], fd)) FD_SET(fd, &native[n]);
    }
    if (!ready()) return -1;
    int result = select(count, reads ? &native[0] : NULL,
                         writes ? &native[1] : NULL,
                         exceptions ? &native[2] : NULL, time_pointer);
    if (result >= 0) {
        for (int n = 0; n < 3; ++n) if (guest[n]) {
            uint64_t output[16] = {0};
            for (int fd = 0; fd < count; ++fd)
                if (FD_ISSET(fd, &native[n])) output[fd / 64] |= UINT64_C(1) << (fd % 64);
            memcpy(guest[n], output, ((size_t)count + 63) / 64 * 8);
        }
        if (timeout) {
            timeout[0] = native_time.tv_sec;
            timeout[1] = native_time.tv_usec;
        }
    }
    return result;
}

bool android_socket_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so"))
        return false;
    *address = 0;
#define MAP(symbol, target) if (!strcmp(name, symbol)) *address = (uint64_t)(uintptr_t)&target; else
    MAP("getnameinfo", guest_getnameinfo)
    MAP("poll", guest_poll)
    MAP("select", guest_select)
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
    MAP("send", guest_send)
    MAP("recv", guest_recv)
    MAP("setsockopt", guest_setsockopt)
    MAP("getsockopt", guest_getsockopt)
    MAP("inet_pton", guest_inet_pton)
    MAP("inet_ntop", guest_inet_ntop)
    MAP("getaddrinfo", guest_getaddrinfo)
    MAP("freeaddrinfo", guest_freeaddrinfo)
    MAP("gai_strerror", guest_gai_strerror)
    { return false; }
#undef MAP
    return true;
}
