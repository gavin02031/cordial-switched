#include "engine_tls_thread.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_THREAD_TLS_BYTES = 1024 * 1024 };

bool engine_tls_thread_prepare(const EngineTlsScratch *template_image,
                               void *opaque_identity, EngineTlsThread *thread)
{
    if (!thread)
        return false;
    memset(thread, 0, sizeof(*thread));
    if (!template_image || !template_image->data ||
        !template_image->size || template_image->size > MAX_THREAD_TLS_BYTES ||
        template_image->alignment < sizeof(void *) ||
        template_image->alignment > MAX_THREAD_TLS_BYTES ||
        (template_image->alignment & (template_image->alignment - 1)) ||
        template_image->size > SIZE_MAX - (template_image->alignment - 1))
        return false;

    const size_t alignment = template_image->alignment;
    void *allocation = malloc(template_image->size + alignment - 1);
    if (!allocation)
        return false;
    const uintptr_t base = (uintptr_t)allocation;
    if (base > UINTPTR_MAX - (alignment - 1)) {
        free(allocation);
        return false;
    }
    unsigned char *data = (unsigned char *)
        ((base + alignment - 1) & ~(uintptr_t)(alignment - 1));
    memcpy(data, template_image->data, template_image->size);

    thread->allocation = allocation;
    thread->data = data;
    thread->size = template_image->size;
    thread->alignment = alignment;
    thread->dtv.generation = 1;
    thread->dtv.modules[1] = data;
    android_arm64_tcb_shadow_init(&thread->tcb, opaque_identity);
    thread->tcb.dtv = &thread->dtv;
    return true;
}

unsigned char *engine_tls_thread_module1_at(EngineTlsThread *thread,
                                             size_t offset, size_t bytes)
{
    if (!thread || !thread->data || thread->dtv.modules[1] != thread->data ||
        offset > thread->size || bytes > thread->size - offset)
        return NULL;
    return thread->data + offset;
}

void engine_tls_thread_release(EngineTlsThread *thread)
{
    if (!thread)
        return;
    free(thread->allocation);
    memset(thread, 0, sizeof(*thread));
}
