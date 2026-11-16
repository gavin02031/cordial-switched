#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "android_socket_compat.h"

typedef int (*SocketFn)(int, int, int);
typedef int (*AddressFn)(int, const void *, uint32_t);
typedef int (*NameFn)(int, void *, uint32_t *);
typedef int (*ListenFn)(int, int);
typedef int (*AcceptFn)(int, void *, uint32_t *);
typedef int (*ShutdownFn)(int, int);
typedef ssize_t (*SendFn)(int, const void *, size_t, int, const void *, uint32_t);
typedef ssize_t (*RecvFn)(int, void *, size_t, int, void *, uint32_t *);
typedef ssize_t (*SendSimpleFn)(int, const void *, size_t, int);
typedef ssize_t (*RecvSimpleFn)(int, void *, size_t, int);
typedef int (*SetOptionFn)(int, int, int, const void *, uint32_t);
typedef int (*GetOptionFn)(int, int, int, void *, uint32_t *);
typedef int (*ParseIpFn)(int, const char *, void *);
typedef const char *(*FormatIpFn)(int, const void *, char *, uint32_t);

// Keep the Bionic layout in this test independent of the implementation.
typedef struct GuestAddrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    uint32_t ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct GuestAddrinfo *ai_next;
} GuestAddrinfo;
typedef int (*GetaddrinfoFn)(const char *, const char *,
                             const GuestAddrinfo *, GuestAddrinfo **);
typedef void (*FreeaddrinfoFn)(GuestAddrinfo *);
typedef const char *(*GaiErrorFn)(int);

static uint64_t symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_socket_compat_resolve(name, "LIBC", "libc.so", &address));
    assert(address != 0);
    return address;
}

// Layout of Bionic/Linux struct sockaddr_in, independent of host libc.
static void guest_loopback(unsigned char address[16])
{
    memset(address, 0, 16);
    address[0] = 2; // AF_INET: uint16_t, little endian on AArch64
    address[4] = 127;
    address[7] = 1;
}

static void negative_tests(void)
{
    uint64_t ignored = 123;
    assert(!android_socket_compat_resolve("socket", "LIBC", "libm.so", &ignored));
    assert(!android_socket_compat_resolve("socket", "GLIBC_2.2.5", "libc.so", &ignored));
    assert(!android_socket_compat_resolve("getrandom", "LIBC", "libc.so", &ignored));
    assert(!android_socket_compat_resolve(NULL, "LIBC", "libc.so", &ignored));

    SocketFn open_socket = (SocketFn)(uintptr_t)symbol("socket");
    AddressFn bind_socket = (AddressFn)(uintptr_t)symbol("bind");
    SendFn send_socket = (SendFn)(uintptr_t)symbol("sendto");
    assert(open_socket(999, 1, 0) == -1 && errno == EAFNOSUPPORT);
    assert(open_socket(2, 1 | 00004000, 0) == -1 && errno == EOPNOTSUPP);
    assert(open_socket(2, 02000000 | 1, 0) == -1 && errno == EOPNOTSUPP);
    unsigned char bad_address[16] = {0};
    assert(bind_socket(-1, bad_address, sizeof(bad_address)) == -1);
    assert(errno == EAFNOSUPPORT);
    bad_address[0] = 2;
    assert(bind_socket(-1, bad_address, 7) == -1 && errno == EINVAL);
    assert(send_socket(-1, NULL, 0, 0x200000, NULL, 0) == -1);
    assert(errno == EOPNOTSUPP);
}

static void tcp_test(void)
{
    SocketFn open_socket = (SocketFn)(uintptr_t)symbol("socket");
    AddressFn bind_socket = (AddressFn)(uintptr_t)symbol("bind");
    AddressFn connect_socket = (AddressFn)(uintptr_t)symbol("connect");
    ListenFn listen_socket = (ListenFn)(uintptr_t)symbol("listen");
    AcceptFn accept_socket = (AcceptFn)(uintptr_t)symbol("accept");
    NameFn getsockname_socket = (NameFn)(uintptr_t)symbol("getsockname");
    NameFn getpeername_socket = (NameFn)(uintptr_t)symbol("getpeername");
    ShutdownFn shutdown_socket = (ShutdownFn)(uintptr_t)symbol("shutdown");
    SendFn send_socket = (SendFn)(uintptr_t)symbol("sendto");
    RecvFn recv_socket = (RecvFn)(uintptr_t)symbol("recvfrom");
    SendSimpleFn send_simple = (SendSimpleFn)(uintptr_t)symbol("send");
    RecvSimpleFn recv_simple = (RecvSimpleFn)(uintptr_t)symbol("recv");
    SetOptionFn set_option = (SetOptionFn)(uintptr_t)symbol("setsockopt");
    GetOptionFn get_option = (GetOptionFn)(uintptr_t)symbol("getsockopt");
    int listener = open_socket(2, 1, 0);
    assert(listener >= 0);
    int reuse = 1;
    assert(set_option(listener, 1, 2, &reuse, sizeof(reuse)) == 0);
    reuse = 0;
    uint32_t option_length = sizeof(reuse);
    assert(get_option(listener, 1, 2, &reuse, &option_length) == 0);
    assert(option_length == sizeof(reuse) && reuse == 1);
    assert(set_option(listener, 1, 4, &reuse, sizeof(reuse)) == -1);
    assert(errno == ENOPROTOOPT); // SO_ERROR needs Android errno remapping
    assert(set_option(listener, 1, 2, &reuse, 1) == -1 && errno == EINVAL);
    unsigned char loopback[16];
    guest_loopback(loopback);
    assert(bind_socket(listener, loopback, sizeof(loopback)) == 0);
    assert(listen_socket(listener, 2) == 0);
    uint32_t length = sizeof(loopback);
    assert(getsockname_socket(listener, loopback, &length) == 0);
    assert(length == sizeof(loopback) && loopback[0] == 2 && loopback[1] == 0);
    assert(loopback[2] != 0 || loopback[3] != 0); // ephemeral port

    int client = open_socket(2, 1, 0);
    assert(client >= 0);
    assert(connect_socket(client, loopback, sizeof(loopback)) == 0);
    unsigned char peer[16] = {0};
    length = sizeof(peer);
    int accepted = accept_socket(listener, peer, &length);
    assert(accepted >= 0 && length == sizeof(peer) && peer[0] == 2);
    length = sizeof(peer);
    assert(getpeername_socket(client, peer, &length) == 0);
    assert(length == sizeof(peer) && !memcmp(peer, loopback, sizeof(peer)));
    assert(send_socket(client, "morrow", 6, 0, NULL, 0) == 6);
    char text[16] = {0};
    assert(recv_socket(accepted, text, sizeof(text), 0, NULL, NULL) == 6);
    assert(!memcmp(text, "morrow", 6));
    assert(send_simple(accepted, "reply", 5, 0) == 5);
    memset(text, 0, sizeof(text));
    assert(recv_simple(client, text, sizeof(text), 0) == 5);
    assert(!memcmp(text, "reply", 5));
    assert(shutdown_socket(client, 1) == 0);
    close(accepted);
    close(client);
    close(listener);
}

static void udp_test(void)
{
    SocketFn open_socket = (SocketFn)(uintptr_t)symbol("socket");
    AddressFn bind_socket = (AddressFn)(uintptr_t)symbol("bind");
    NameFn getsockname_socket = (NameFn)(uintptr_t)symbol("getsockname");
    SendFn send_socket = (SendFn)(uintptr_t)symbol("sendto");
    RecvFn recv_socket = (RecvFn)(uintptr_t)symbol("recvfrom");
    int receiver = open_socket(2, 2, 0);
    int sender = open_socket(2, 2, 0);
    assert(receiver >= 0 && sender >= 0);
    unsigned char loopback[16];
    guest_loopback(loopback);
    assert(bind_socket(receiver, loopback, sizeof(loopback)) == 0);
    uint32_t length = sizeof(loopback);
    assert(getsockname_socket(receiver, loopback, &length) == 0 && length == 16);
    assert(send_socket(sender, "udp", 3, 0, loopback, sizeof(loopback)) == 3);
    char text[8] = {0};
    unsigned char from[16] = {0};
    uint32_t from_length = sizeof(from);
    assert(recv_socket(receiver, text, sizeof(text), 2, from, &from_length) == 3);
    assert(!memcmp(text, "udp", 3) && from[0] == 2 && from_length == 16);
    memset(text, 0, sizeof(text));
    assert(recv_socket(receiver, text, sizeof(text), 0, NULL, NULL) == 3);
    assert(!memcmp(text, "udp", 3));
    close(sender);
    close(receiver);
}

static void ipv6_test(void)
{
    SocketFn open_socket = (SocketFn)(uintptr_t)symbol("socket");
    AddressFn bind_socket = (AddressFn)(uintptr_t)symbol("bind");
    NameFn getsockname_socket = (NameFn)(uintptr_t)symbol("getsockname");
    SendFn send_socket = (SendFn)(uintptr_t)symbol("sendto");
    RecvFn recv_socket = (RecvFn)(uintptr_t)symbol("recvfrom");
    int receiver = open_socket(10, 2, 0); // Bionic AF_INET6 = 10 (BSD = 28)
    if (receiver < 0 && (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT)) {
        puts("IPv6 unavailable on host; skipping IPv6 loopback");
        return;
    }
    assert(receiver >= 0);
    unsigned char loopback[28] = {0};
    loopback[0] = 10;
    loopback[23] = 1; // ::1
    const int bind_result = bind_socket(receiver, loopback, sizeof(loopback));
    if (bind_result < 0 && (errno == EADDRNOTAVAIL || errno == ENETUNREACH)) {
        puts("IPv6 loopback unavailable on host; skipping IPv6 loopback");
        close(receiver);
        return;
    }
    assert(bind_result == 0);
    uint32_t length = sizeof(loopback);
    assert(getsockname_socket(receiver, loopback, &length) == 0);
    assert(length == 28 && loopback[0] == 10 && loopback[23] == 1);
    int sender = open_socket(10, 2, 0);
    assert(sender >= 0);
    assert(send_socket(sender, "v6", 2, 0, loopback, sizeof(loopback)) == 2);
    char text[8] = {0};
    unsigned char from[28] = {0};
    uint32_t from_length = sizeof(from);
    assert(recv_socket(receiver, text, sizeof(text), 0, from, &from_length) == 2);
    assert(from_length == 28 && from[0] == 10 && from[23] == 1);
    assert(!memcmp(text, "v6", 2));
    close(sender);
    close(receiver);
}

static void dns_test(void)
{
    GetaddrinfoFn resolve = (GetaddrinfoFn)(uintptr_t)symbol("getaddrinfo");
    FreeaddrinfoFn release = (FreeaddrinfoFn)(uintptr_t)symbol("freeaddrinfo");
    GaiErrorFn error_text = (GaiErrorFn)(uintptr_t)symbol("gai_strerror");
    GuestAddrinfo hints = {0};
    GuestAddrinfo *result = NULL;
    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_flags = 0x4 | 0x8; // Bionic AI_NUMERICHOST | AI_NUMERICSERV
    assert(resolve("127.0.0.1", "443", &hints, &result) == 0);
    assert(result && result->ai_family == 2 && result->ai_socktype == 1);
    assert(result->ai_addrlen == 16 && result->ai_addr);
    const unsigned char *ip = result->ai_addr;
    assert(ip[0] == 2 && ip[1] == 0 && ip[2] == 1 && ip[3] == 187);
    assert(ip[4] == 127 && ip[5] == 0 && ip[6] == 0 && ip[7] == 1);
    release(result);
    release(NULL);

    result = NULL;
    hints.ai_family = 10;
    assert(resolve("::1", "8080", &hints, &result) == 0);
    assert(result && result->ai_family == 10 && result->ai_addrlen == 28);
    ip = result->ai_addr;
    assert(ip[0] == 10 && ip[1] == 0 && ip[2] == 31 && ip[3] == 144);
    assert(ip[23] == 1);
    release(result);

    hints.ai_family = 999;
    assert(resolve("127.0.0.1", "80", &hints, &result) == 5);
    hints.ai_family = 2;
    hints.ai_flags = 0x1000000;
    assert(resolve("127.0.0.1", "80", &hints, &result) == 3);
    hints.ai_flags = 0;
    hints.ai_socktype = 999;
    assert(resolve("127.0.0.1", "80", &hints, &result) == 10);
    hints.ai_socktype = 1;
    hints.ai_protocol = 999;
    assert(resolve("127.0.0.1", "80", &hints, &result) == 13);
    hints.ai_protocol = 0;
    hints.ai_addrlen = 16;
    assert(resolve("127.0.0.1", "80", &hints, &result) == 12);
    hints.ai_addrlen = 0;
    assert(resolve(NULL, NULL, &hints, &result) == 8);
    assert(resolve("127.0.0.1", "80", &hints, NULL) == 11 && errno == EFAULT);
    assert(strlen(error_text(3)) && strlen(error_text(12345)));
}

static void ip_text_test(void)
{
    ParseIpFn parse = (ParseIpFn)(uintptr_t)symbol("inet_pton");
    FormatIpFn format = (FormatIpFn)(uintptr_t)symbol("inet_ntop");
    unsigned char raw[16] = {0};
    char text[48] = {0};
    assert(parse(2, "127.0.0.1", raw) == 1);
    assert(raw[0] == 127 && raw[3] == 1);
    assert(format(2, raw, text, sizeof(text)) == text);
    assert(!strcmp(text, "127.0.0.1"));
    memset(raw, 0, sizeof(raw));
    assert(parse(10, "::1", raw) == 1);
    assert(raw[15] == 1);
    assert(format(10, raw, text, sizeof(text)) == text);
    assert(!strcmp(text, "::1"));
    assert(parse(999, "127.0.0.1", raw) == -1 && errno == EAFNOSUPPORT);
    assert(parse(2, "invalid", raw) == 0);
}

int main(void)
{
    negative_tests();
    tcp_test();
    udp_test();
    ipv6_test();
    dns_test();
    ip_text_test();
    puts("Android socket ABI tests passed (IPv4 TCP/UDP, IPv6, DNS, options, text)");
    return 0;
}
