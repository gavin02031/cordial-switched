#ifndef CORDIAL_ENGINE_TLS_THREAD_H
#define CORDIAL_ENGINE_TLS_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "android_arm64_tcb_shadow.h"
#include "engine_tls_scratch.h"

// One Android ELF module's isolated TLS storage on one native thread. This
// provides a real per-thread copy and a diagnostic DTV image; it does not
// install TPIDR_EL0, implement dynamic module generations or provide a Bionic
// pthread_internal_t. In particular it must not be passed to Roblox yet.
typedef struct {
    uint64_t generation;
    unsigned char *modules[2]; // Slot 0 unused; the isolated ELF is module 1.
} EngineTlsSingleDtv;

typedef struct {
    void *allocation;
    unsigned char *data;
    size_t size;
    size_t alignment;
    EngineTlsSingleDtv dtv;
    AndroidArm64TcbShadow tcb;
} EngineTlsThread;

// The template remains owned by the caller. A successful result owns a fresh
// aligned allocation, independent of both the template and other threads.
bool engine_tls_thread_prepare(const EngineTlsScratch *template_image,
                               void *opaque_identity, EngineTlsThread *thread);
// Strictly bounded slot-1 lookup for diagnostics and host tests, not an
// implementation of Bionic's __tls_get_addr or TLSDESC resolver.
unsigned char *engine_tls_thread_module1_at(EngineTlsThread *thread,
                                             size_t offset, size_t bytes);
void engine_tls_thread_release(EngineTlsThread *thread);

#endif
