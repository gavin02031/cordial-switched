#include "android_arm64_tcb_shadow.h"

#include <stddef.h>
#include <string.h>

// Source: AOSP bionic libc/platform/bionic/tls_defines.h, ARM64 variant.
_Static_assert(sizeof(void *) == 8, "Bionic ARM64 shadow requires 64-bit pointers");
_Static_assert(sizeof(AndroidArm64TcbShadow) == 9 * sizeof(void *), "unexpected TCB span");
_Static_assert(offsetof(AndroidArm64TcbShadow, bionic_tls) == 0, "TP[-1] offset");
_Static_assert(offsetof(AndroidArm64TcbShadow, dtv) == 8, "TP[0] offset");
_Static_assert(offsetof(AndroidArm64TcbShadow, thread_id) == 16, "TP[1] offset");
_Static_assert(offsetof(AndroidArm64TcbShadow, stack_guard) == 48, "TP[5] offset");
_Static_assert(offsetof(AndroidArm64TcbShadow, art_thread_self) == 64, "TP[7] offset");

void android_arm64_tcb_shadow_init(AndroidArm64TcbShadow *tcb, void *identity)
{
    if (!tcb)
        return;
    memset(tcb, 0, sizeof(*tcb));
    // The opaque bridge identity is not a Bionic pthread_internal_t. Only
    // the relative location of its slot is established by this experiment.
    tcb->thread_id = identity;
}

const void *const *android_arm64_tcb_shadow_pointer(const AndroidArm64TcbShadow *tcb)
{
    return tcb ? (const void *const *)&tcb->dtv : NULL;
}
