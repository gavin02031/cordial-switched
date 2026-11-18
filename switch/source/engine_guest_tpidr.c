#include "engine_guest_tpidr.h"

#ifdef __SWITCH__

uint64_t engine_guest_tpidr_install(const void *guest_tcb)
{
    uint64_t saved;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(saved));
    __asm__ volatile("msr tpidr_el0, %0" :: "r"((uint64_t)guest_tcb) : "memory");
    return saved;
}

void engine_guest_tpidr_restore(uint64_t saved_host_tpidr)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(saved_host_tpidr) : "memory");
}

#else

// Host model for tests: records install/restore symmetry only. There is no
// privileged register to touch, and no host code path may depend on this
// modeling real thread-pointer behavior.
static const void *host_installed;

uint64_t engine_guest_tpidr_install(const void *guest_tcb)
{
    host_installed = guest_tcb;
    return 0; // Modeled host token; the Switch build returns the real TPIDR.
}

void engine_guest_tpidr_restore(uint64_t saved_host_tpidr)
{
    (void)saved_host_tpidr;
    host_installed = 0;
}

const void *engine_guest_tpidr_installed_for_test(void)
{
    return host_installed;
}

#endif
