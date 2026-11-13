#define _POSIX_C_SOURCE 200809L
#include "android_tls_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// AOSP Bionic LP64 uses 32-bit keys with the high bit set; PTHREAD_KEYS_MAX
// is 128 and a key's sequence changes on delete/reuse. The user-visible value
// is an index, not an underlying pthread_key_t from the host implementation.
// See platform/bionic/libc/bionic/pthread_key.cpp and libc/include/limits.h.
// Existing Switch Android ports demonstrate multiplexing many Bionic keys
// into one native pthread TLS slot when libnx exposes fewer native slots.
enum { ANDROID_KEY_COUNT = 128, ANDROID_KEY_DESTRUCTOR_ROUNDS = 4 };
enum { ANDROID_KEY_FLAG = 0x80000000u };

typedef struct {
    uint32_t sequence;
    void (*destructor)(void *);
} KeySlot;

typedef struct {
    void *values[ANDROID_KEY_COUNT];
    uint32_t sequences[ANDROID_KEY_COUNT];
} ThreadValues;

static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t native_master_key;
static bool master_ready;
static KeySlot key_slots[ANDROID_KEY_COUNT];

static bool decode_key(uint32_t key, unsigned *index)
{
    if ((key & ANDROID_KEY_FLAG) == 0 ||
        (key & ~ANDROID_KEY_FLAG) >= ANDROID_KEY_COUNT)
        return false;
    *index = key & ~ANDROID_KEY_FLAG;
    return true;
}

static void master_destructor(void *opaque)
{
    ThreadValues *thread = opaque;
    if (!thread) return;

    // A native pthread implementation clears its key before calling the
    // destructor. Restore it temporarily so a Bionic destructor can call
    // pthread_setspecific and request another iteration, as POSIX permits.
    if (pthread_setspecific(native_master_key, thread) != 0) {
        free(thread);
        return;
    }
    for (unsigned round = 0; round < ANDROID_KEY_DESTRUCTOR_ROUNDS; round++) {
        unsigned called = 0;
        for (unsigned i = 0; i < ANDROID_KEY_COUNT; i++) {
            void (*destructor)(void *) = NULL;
            void *value = NULL;
            pthread_mutex_lock(&key_lock);
            KeySlot *slot = &key_slots[i];
            if ((slot->sequence & 1u) != 0 &&
                thread->sequences[i] == slot->sequence &&
                thread->values[i] && slot->destructor) {
                destructor = slot->destructor;
                value = thread->values[i];
                thread->values[i] = NULL;
            }
            pthread_mutex_unlock(&key_lock);
            // Never call user-provided destructors with the registry locked:
            // destructors may create/delete keys or call setspecific again.
            if (destructor) {
                destructor(value);
                called++;
            }
        }
        if (!called) break;
    }
    pthread_setspecific(native_master_key, NULL);
    free(thread);
}

static int android_key_create(uint32_t *out, void (*destructor)(void *))
{
    if (!out) return EINVAL;
    pthread_mutex_lock(&key_lock);
    if (!master_ready) {
        int error = pthread_key_create(&native_master_key, master_destructor);
        if (error) {
            pthread_mutex_unlock(&key_lock);
            return error;
        }
        master_ready = true;
    }
    for (unsigned i = 0; i < ANDROID_KEY_COUNT; i++) {
        if ((key_slots[i].sequence & 1u) == 0) {
            key_slots[i].sequence++;
            key_slots[i].destructor = destructor;
            *out = ANDROID_KEY_FLAG | i;
            pthread_mutex_unlock(&key_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&key_lock);
    return EAGAIN;
}

static int android_key_delete(uint32_t key)
{
    unsigned index;
    if (!decode_key(key, &index)) return EINVAL;
    pthread_mutex_lock(&key_lock);
    KeySlot *slot = &key_slots[index];
    if ((slot->sequence & 1u) == 0) {
        pthread_mutex_unlock(&key_lock);
        return EINVAL;
    }
    slot->destructor = NULL;
    slot->sequence++;
    pthread_mutex_unlock(&key_lock);
    return 0;
}

static void *android_getspecific(uint32_t key)
{
    unsigned index;
    if (!decode_key(key, &index)) return NULL;
    pthread_mutex_lock(&key_lock);
    void *value = NULL;
    if (master_ready) {
        ThreadValues *thread = pthread_getspecific(native_master_key);
        if (thread) {
            const uint32_t sequence = key_slots[index].sequence;
            if ((sequence & 1u) && thread->sequences[index] == sequence)
                value = thread->values[index];
            else
                thread->values[index] = NULL;
        }
    }
    pthread_mutex_unlock(&key_lock);
    return value;
}

static int android_setspecific(uint32_t key, const void *value)
{
    unsigned index;
    if (!decode_key(key, &index)) return EINVAL;
    pthread_mutex_lock(&key_lock);
    if (!master_ready || !(key_slots[index].sequence & 1u)) {
        pthread_mutex_unlock(&key_lock);
        return EINVAL;
    }
    ThreadValues *thread = pthread_getspecific(native_master_key);
    if (!thread) {
        thread = calloc(1, sizeof(*thread));
        if (!thread) {
            pthread_mutex_unlock(&key_lock);
            return ENOMEM;
        }
        int error = pthread_setspecific(native_master_key, thread);
        if (error) {
            free(thread);
            pthread_mutex_unlock(&key_lock);
            return error;
        }
    }
    thread->sequences[index] = key_slots[index].sequence;
    thread->values[index] = (void *)value;
    pthread_mutex_unlock(&key_lock);
    return 0;
}

bool android_tls_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
    if (strcmp(name, "pthread_key_create") == 0)
        *address = (uint64_t)(uintptr_t)&android_key_create;
    else if (strcmp(name, "pthread_key_delete") == 0)
        *address = (uint64_t)(uintptr_t)&android_key_delete;
    else if (strcmp(name, "pthread_getspecific") == 0)
        *address = (uint64_t)(uintptr_t)&android_getspecific;
    else if (strcmp(name, "pthread_setspecific") == 0)
        *address = (uint64_t)(uintptr_t)&android_setspecific;
    return *address != 0;
}
