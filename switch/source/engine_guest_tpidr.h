#ifndef CORDIAL_ENGINE_GUEST_TPIDR_H
#define CORDIAL_ENGINE_GUEST_TPIDR_H

#include <stdint.h>

// The guest's Bionic thread pointer lives in TPIDR_EL0. Host thread state on
// this toolchain lives on TPIDRRO_EL0 - verified against libnx source:
// __aarch64_read_tp (the compiler's TLS helper, nx/source/runtime/readtp.s)
// and armGetTls (nx/include/switch/arm/tls.h) both read tpidrro_el0, and
// newlib's per-thread reent is served from libnx TLS. Installing the guest
// shadow TCB in TPIDR_EL0 therefore does not collide with host errno or
// compiler TLS. The console run remains the full-system validation: any
// prebuilt object reading TPIDR_EL0 directly would still collide.

// Save the current TPIDR_EL0 and install the guest shadow TCB pointer.
// The returned token must be handed to engine_guest_tpidr_restore exactly
// once, on the same thread, after the guest call sequence completes.
uint64_t engine_guest_tpidr_install(const void *guest_tcb);
void engine_guest_tpidr_restore(uint64_t saved_host_tpidr);

#ifndef __SWITCH__
// Host-test observability for the model shim: the pointer most recently
// installed and not yet restored, or NULL. The Switch build installs a real
// register and offers no such accessor.
const void *engine_guest_tpidr_installed_for_test(void);
#endif

#endif
