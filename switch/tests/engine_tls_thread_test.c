#include "engine_tls_thread.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { WORKERS = 16, TLS_BYTES = 128 };

typedef struct {
    EngineTlsScratch *template_image;
    unsigned index;
    unsigned observed;
} Worker;

static void *worker_main(void *opaque)
{
    Worker *worker = opaque;
    EngineTlsThread thread = {0};
    assert(engine_tls_thread_prepare(worker->template_image, worker, &thread));
    assert((uintptr_t)thread.data % 64 == 0);
    assert(thread.dtv.generation == 1 && thread.dtv.modules[0] == NULL);
    assert(thread.dtv.modules[1] == thread.data);
    const void *const *tp = android_arm64_tcb_shadow_pointer(&thread.tcb);
    assert(tp[0] == &thread.dtv && tp[1] == worker);
    assert(tp[-1] == NULL); // No Bionic TLS runtime or TPIDR_EL0 switch.
    assert(*engine_tls_thread_module1_at(&thread, 0, 1) == 0x5a);
    assert(*engine_tls_thread_module1_at(&thread, 5, 1) == 0);
    *engine_tls_thread_module1_at(&thread, 5, 1) = (unsigned char)worker->index;
    worker->observed = *engine_tls_thread_module1_at(&thread, 5, 1);
    assert(engine_tls_thread_module1_at(&thread, TLS_BYTES, 1) == NULL);
    assert(engine_tls_thread_module1_at(&thread, SIZE_MAX, 1) == NULL);
    assert(engine_tls_thread_module1_at(&thread, TLS_BYTES, 0) == thread.data + TLS_BYTES);
    engine_tls_thread_release(&thread);
    assert(thread.data == NULL && thread.dtv.modules[1] == NULL);
    return NULL;
}

int main(void)
{
    unsigned char source[TLS_BYTES] = {0};
    source[0] = 0x5a;
    source[1] = 0xa5;
    EngineTlsScratch original = {
        .allocation = NULL, .data = source, .size = sizeof(source), .alignment = 64,
    };
    EngineTlsThread main_thread = {0};
    assert(engine_tls_thread_prepare(&original, NULL, &main_thread));
    assert(main_thread.tcb.thread_id == NULL);
    assert(memcmp(main_thread.data, source, TLS_BYTES) == 0);
    main_thread.data[1] = 0xff;
    assert(source[1] == 0xa5); // A thread never borrows the shared initializer.

    pthread_t threads[WORKERS];
    Worker workers[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        workers[i] = (Worker){.template_image = &original, .index = i + 1};
        assert(pthread_create(&threads[i], NULL, worker_main, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < WORKERS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(workers[i].observed == i + 1);
    }
    assert(source[5] == 0 && main_thread.data[5] == 0);

    EngineTlsScratch bad = original;
    EngineTlsThread failed = {0};
    bad.alignment = 3;
    assert(!engine_tls_thread_prepare(&bad, NULL, &failed));
    bad = original;
    bad.alignment = 2 * 1024 * 1024;
    assert(!engine_tls_thread_prepare(&bad, NULL, &failed));
    bad = original;
    bad.data = NULL;
    assert(!engine_tls_thread_prepare(&bad, NULL, &failed));
    assert(!engine_tls_thread_prepare(NULL, NULL, &failed));
    assert(!engine_tls_thread_prepare(&original, NULL, NULL));
    engine_tls_thread_release(&main_thread);
    engine_tls_thread_release(&failed);
    engine_tls_thread_release(NULL);
    puts("engine_tls_thread_test: distinct aligned per-thread images and DTV shadows passed");
    return 0;
}
