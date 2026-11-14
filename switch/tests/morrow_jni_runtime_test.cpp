#include <string> // The pinned libjnivm headers need this before unordered_map.
#include <jnivm.h>

#include "android_jni_vm_bridge.h"
#include "morrow_jni_runtime.h"

#include <cassert>
#include <cstdio>
#include <pthread.h>

// Exercise the actual JNI function table rather than a mock provider. The
// Switch JNI hardware probe separately tests whether this survives libnx.
struct BridgeVm;
struct BridgeFunctions {
    void *reserved0, *reserved1, *reserved2;
    int32_t (*destroy)(BridgeVm *);
    int32_t (*attach)(BridgeVm *, void **, void *);
    int32_t (*detach)(BridgeVm *);
    int32_t (*get_env)(BridgeVm *, void **, int32_t);
    int32_t (*attach_daemon)(BridgeVm *, void **, void *);
};
struct BridgeVm { const BridgeFunctions *functions; };

struct WorkerResult {
    BridgeVm *bridge;
    JNIEnv *main_env;
    JavaVM *native_vm;
    JNIEnv *worker_env;
};

static void *worker_run(void *value)
{
    auto *worker = static_cast<WorkerResult *>(value);
    auto *f = worker->bridge->functions;
    void *env = reinterpret_cast<void *>(1);
    assert(f->get_env(worker->bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_EDETACHED && !env);
    assert(f->attach_daemon(worker->bridge, &env, nullptr) == ANDROID_JNI_OK);
    worker->worker_env = static_cast<JNIEnv *>(env);
    assert(worker->worker_env && worker->worker_env != worker->main_env);
    JavaVM *returned_vm = nullptr;
    assert(worker->worker_env->GetJavaVM(&returned_vm) == JNI_OK);
    assert(returned_vm == worker->native_vm);
    assert(f->get_env(worker->bridge, &env, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_OK);
    assert(env == worker->worker_env);
    assert(f->detach(worker->bridge) == ANDROID_JNI_OK);
    assert(f->get_env(worker->bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_EDETACHED && !env);
    assert(f->detach(worker->bridge) == ANDROID_JNI_EDETACHED);
    return nullptr;
}

int main()
{
    auto *bridge = reinterpret_cast<BridgeVm *>(android_jni_vm_bridge_instance());
    assert(bridge && bridge->functions);
    assert(!android_jni_vm_bridge_has_provider());
    assert(morrow_jni_runtime_java_vm() == nullptr);
    assert(morrow_jni_runtime_start());
    assert(morrow_jni_runtime_start());
    assert(android_jni_vm_bridge_has_provider());
    auto *native_vm = static_cast<JavaVM *>(morrow_jni_runtime_java_vm());
    assert(native_vm);
    void *env = nullptr;
    assert(bridge->functions->get_env(bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_OK);
    auto *main_env = static_cast<JNIEnv *>(env);
    assert(main_env && main_env->GetVersion() == JNI_VERSION_1_6);
    JavaVM *returned_vm = nullptr;
    assert(main_env->GetJavaVM(&returned_vm) == JNI_OK);
    assert(returned_vm == native_vm);
    assert(bridge->functions->get_env(bridge, &env, 0x00010008) ==
           ANDROID_JNI_EVERSION && !env);

    WorkerResult worker = {bridge, main_env, native_vm, nullptr};
    pthread_t thread;
    assert(pthread_create(&thread, nullptr, worker_run, &worker) == 0);
    assert(pthread_join(thread, nullptr) == 0);
    assert(worker.worker_env && worker.worker_env != main_env);
    assert(bridge->functions->get_env(bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_OK && env == main_env);
    puts("morrow_jni_runtime_test: JNI provider and per-thread identity passed");
    return 0;
}
