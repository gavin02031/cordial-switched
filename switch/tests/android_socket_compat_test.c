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
    int listener = open_socket(2, 1, 0);
    assert(listener >= 0);
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

int main(void)
{
    negative_tests();
    tcp_test();
    udp_test();
    ipv6_test();
    puts("Android socket ABI tests passed (negative cases, IPv4 TCP/UDP, IPv6)");
    return 0;
}
