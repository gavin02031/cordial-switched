#include "android_jni_vm_bridge.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

// Mirror JNI's public JavaVM C vtable to exercise the actual ABI slots rather
// than testing the implementation's private static functions.
typedef struct TestJavaVM TestJavaVM;
typedef struct {
    void *reserved0, *reserved1, *reserved2;
    int32_t (*DestroyJavaVM)(TestJavaVM *);
    int32_t (*AttachCurrentThread)(TestJavaVM *, void **, void *);
    int32_t (*DetachCurrentThread)(TestJavaVM *);
    int32_t (*GetEnv)(TestJavaVM *, void **, int32_t);
    int32_t (*AttachCurrentThreadAsDaemon)(TestJavaVM *, void **, void *);
} TestJniInvokeTable;
struct TestJavaVM { const TestJniInvokeTable *functions; };

typedef struct {
    atomic_int attaches;
    atomic_int daemon_attaches;
    atomic_int detaches;
    atomic_bool return_broken_env;
} FakeProviderState;

static _Thread_local int local_environment_token;
static _Thread_local void *local_environment;

static int32_t fake_get_env(void *context, void **out, int32_t version)
{
    FakeProviderState *state = context;
    if (atomic_load(&state->return_broken_env)) return ANDROID_JNI_OK;
    if (version != ANDROID_JNI_VERSION_1_6) return ANDROID_JNI_EVERSION;
    *out = local_environment;
    return *out ? ANDROID_JNI_OK : ANDROID_JNI_EDETACHED;
}

static int32_t fake_attach(void *context, void **out, void *arguments, bool daemon)
{
    FakeProviderState *state = context;
    assert(arguments == NULL);
    atomic_fetch_add(&state->attaches, 1);
    if (daemon) atomic_fetch_add(&state->daemon_attaches, 1);
    local_environment = &local_environment_token;
    *out = local_environment;
    return ANDROID_JNI_OK;
}

static int32_t fake_detach(void *context)
{
    FakeProviderState *state = context;
    if (!local_environment) return ANDROID_JNI_EDETACHED;
    atomic_fetch_add(&state->detaches, 1);
    local_environment = NULL;
    return ANDROID_JNI_OK;
}

typedef struct { TestJavaVM *vm; void *thread_environment; } Worker;

static void *worker_thread(void *argument)
{
    Worker *worker = argument;
    void *env = (void *)1;
    assert(worker->vm->functions->GetEnv(worker->vm, &env,
                                        ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_EDETACHED);
    assert(env == NULL); // Parent-thread JNIEnv is never inherited.
    assert(worker->vm->functions->AttachCurrentThreadAsDaemon(worker->vm, &env,
                                                              NULL) == ANDROID_JNI_OK);
    assert(env != NULL);
    worker->thread_environment = env;
    assert(worker->vm->functions->GetEnv(worker->vm, &env,
                                        ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_OK);
    assert(worker->vm->functions->DetachCurrentThread(worker->vm) == ANDROID_JNI_OK);
    assert(worker->vm->functions->GetEnv(worker->vm, &env,
                                        ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_EDETACHED);
    assert(env == NULL);
    return NULL;
}

int main(void)
{
    TestJavaVM *vm = (TestJavaVM *)android_jni_vm_bridge_instance();
    assert(vm && vm->functions);
    assert(!android_jni_vm_bridge_has_provider());
    assert(vm->functions->reserved0 == NULL);
    assert(vm->functions->reserved1 == NULL);
    assert(vm->functions->reserved2 == NULL);

    void *env = (void *)1;
    assert(vm->functions->GetEnv(vm, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_EDETACHED && env == NULL);
    assert(vm->functions->GetEnv(vm, &env, 0x99999999) ==
           ANDROID_JNI_EVERSION && env == NULL);
    assert(vm->functions->GetEnv(vm, NULL, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_ERR);
    assert(vm->functions->AttachCurrentThread(vm, &env, NULL) == ANDROID_JNI_ERR);
    assert(env == NULL);
    assert(vm->functions->AttachCurrentThreadAsDaemon(vm, &env, NULL) == ANDROID_JNI_ERR);
    assert(env == NULL);
    assert(vm->functions->DetachCurrentThread(vm) == ANDROID_JNI_ERR);
    assert(vm->functions->DestroyJavaVM(vm) == ANDROID_JNI_ERR);
    TestJavaVM wrong_vm = { vm->functions };
    assert(vm->functions->GetEnv(&wrong_vm, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_ERR && env == NULL);
    assert(vm->functions->AttachCurrentThread(&wrong_vm, &env, NULL) == ANDROID_JNI_ERR);
    assert(vm->functions->DetachCurrentThread(&wrong_vm) == ANDROID_JNI_ERR);

    FakeProviderState state = {0};
    AndroidJniVmProvider invalid = {0};
    assert(!android_jni_vm_bridge_install_provider(NULL, &state));
    assert(!android_jni_vm_bridge_install_provider(&invalid, &state));
    const AndroidJniVmProvider provider = {
        fake_get_env, fake_attach, fake_detach, NULL
    };
    assert(android_jni_vm_bridge_install_provider(&provider, &state));
    assert(!android_jni_vm_bridge_install_provider(&provider, &state));
    assert(android_jni_vm_bridge_has_provider());
    assert(vm->functions->GetEnv(vm, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_EDETACHED && env == NULL);
    assert(vm->functions->AttachCurrentThread(vm, &env, NULL) == ANDROID_JNI_OK);
    void *main_environment = env;
    assert(main_environment != NULL);
    assert(vm->functions->GetEnv(vm, &env, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_OK);
    assert(env == main_environment);
    assert(vm->functions->GetEnv(vm, &env, 0x00010008) == ANDROID_JNI_EVERSION);
    assert(env == NULL);
    assert(vm->functions->GetEnv(vm, &env, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_OK);

    Worker worker = {vm, NULL};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, worker_thread, &worker) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(worker.thread_environment && worker.thread_environment != main_environment);
    assert(vm->functions->GetEnv(vm, &env, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_OK);
    assert(env == main_environment);
    atomic_store(&state.return_broken_env, true);
    assert(vm->functions->GetEnv(vm, &env, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_ERR);
    assert(env == NULL); // A provider cannot claim success with NULL JNIEnv.
    atomic_store(&state.return_broken_env, false);
    assert(vm->functions->DetachCurrentThread(vm) == ANDROID_JNI_OK);
    assert(vm->functions->DetachCurrentThread(vm) == ANDROID_JNI_EDETACHED);
    assert(vm->functions->DestroyJavaVM(vm) == ANDROID_JNI_ERR);
    assert(atomic_load(&state.attaches) == 2);
    assert(atomic_load(&state.daemon_attaches) == 1);
    assert(atomic_load(&state.detaches) == 2);
    puts("android_jni_vm_bridge_test: all assertions passed (mock provider only)");
    return 0;
}
