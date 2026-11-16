#ifndef CORDIAL_ANDROID_ARM64_TCB_SHADOW_H
#define CORDIAL_ANDROID_ARM64_TCB_SHADOW_H

#include <stddef.h>

// AArch64 Bionic reserves slots -1 through 7 relative to the thread pointer.
// This is only an in-memory diagnostic layout; TPIDR_EL0 is not installed.
// libnx armGetTls uses TPIDRRO_EL0. A guest TPIDR_EL0 bridge still needs
// validation across native callbacks, thread creation and compiler TLS.
typedef struct {
    void *bionic_tls;      // TP[-1]
    void *dtv;             // TP[0]
    void *thread_id;       // TP[1]
    void *app;             // TP[2]
    void *opengl;          // TP[3]
    void *opengl_api;      // TP[4]
    void *stack_guard;     // TP[5]
    void *sanitizer;       // TP[6]
    void *art_thread_self; // TP[7]
} AndroidArm64TcbShadow;

void android_arm64_tcb_shadow_init(AndroidArm64TcbShadow *tcb, void *identity);
const void *const *android_arm64_tcb_shadow_pointer(const AndroidArm64TcbShadow *tcb);

#endif
