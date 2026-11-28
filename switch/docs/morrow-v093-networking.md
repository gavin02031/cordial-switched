# Cordial Switched v0.93: Android networking compatibility

The preview now implements nine additional `libc.so@LIBC` symbols:
`getaddrinfo`, `freeaddrinfo`, `gai_strerror`, `send`, `recv`, `setsockopt`,
`getsockopt`, `inet_pton`, and `inet_ntop`.

DNS results contain guest-owned Bionic LP64 `addrinfo` nodes. The adapter
copies the native address and canonical name into newly allocated memory,
rewrites the IPv4/IPv6 address family and translates native `EAI_*` error codes
to Bionic values. `freeaddrinfo` releases the guest allocations. The native
resolver list never crosses into the Android client. Address lists are capped
at 128 nodes; unsupported hints and address families return explicit errors.

Android's `SOL_SOCKET` and many `SO_*` constants differ from Horizon's BSD
values. Only `SO_REUSEADDR`, `SO_BROADCAST`, `SO_SNDBUF`, `SO_RCVBUF` and
`SO_KEEPALIVE` are currently translated, with the corresponding four-byte
integer representation. Other socket options, including `SO_ERROR`, remain
unresolved through this provider because their return values or structures
require additional ABI translation. IPv4 and IPv6 textual address functions
translate the family constants before calling the native parser or formatter.

The host test covers numeric DNS service/address roundtrips in IPv4 and IPv6,
invalid hints, unknown flags, Bionic error numbering, socket option
translation, reverse IP text and actual localhost TCP/UDP traffic. The
cross-compiled `.nro` is checked separately in GitHub Actions. DNS resolving
or a passing host network test **does not establish that the Roblox engine
initialises, logs in or joins a game**.

**INFERRED / requires a new Switch audit:** these are useful networking
providers, but their names have not been checked against a fresh imported
symbol list from the Switch Lite. The last on-device confirmed baseline is
437 of 629 supported lookups and 192 unresolved in v0.86. The engine still
has other strict linker, JNI, lifecycle and graphics blockers. Keep the
Cordial Switched preview branding until playable Morrow v1.0 is observed.

Sources for the incompatible layouts and constants:

- [Android Bionic `netdb.h`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/include/netdb.h)
- [Android socket option definitions](https://android.googlesource.com/platform/bionic/+/e2aefc46e/libc/kernel/uapi/asm-generic/socket.h)
- [libnx BSD socket definitions](https://github.com/switchbrew/libnx/blob/master/nx/external/bsd/include/sys/socket.h)
