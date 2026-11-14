#include <string> // The pinned libjnivm headers need this before unordered_map.
#include <jnivm.h>

#include "android_jni_vm_bridge.h"
#include "android_native_window_compat.h"
#include "morrow_jni_runtime.h"

#include <cassert>
#include <cstdio>
#include <cstring>
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

using FromSurface = void *(*)(void *, void *);
using WindowDimension = int32_t (*)(void *);
using ReleaseWindow = void (*)(void *);

template<typename Callback> static Callback native_window_symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_native_window_compat_resolve(name, nullptr, nullptr, &address));
    Callback callback = nullptr;
    static_assert(sizeof(callback) == sizeof(address), "pointer width mismatch");
    std::memcpy(&callback, &address, sizeof(callback));
    return callback;
}

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

    auto from_surface = native_window_symbol<FromSurface>("ANativeWindow_fromSurface");
    auto width = native_window_symbol<WindowDimension>("ANativeWindow_getWidth");
    auto height = native_window_symbol<WindowDimension>("ANativeWindow_getHeight");
    auto release = native_window_symbol<ReleaseWindow>("ANativeWindow_release");
    int horizon_window = 1, other_window = 2;
    assert(!morrow_jni_runtime_surface_bound());
    assert(!morrow_jni_runtime_bind_horizon_surface(nullptr, 1280, 720));
    assert(!morrow_jni_runtime_bind_horizon_surface(&horizon_window, 0, 720));
    void *surface = morrow_jni_runtime_bind_horizon_surface(&horizon_window, 1280, 720);
    assert(surface && morrow_jni_runtime_surface_bound());
    assert(main_env->IsInstanceOf(static_cast<jobject>(surface),
                                  main_env->FindClass("android/view/Surface")));
    assert(from_surface(main_env, &horizon_window) == nullptr);
    void *guest = from_surface(main_env, surface);
    void *actual_window = nullptr;
    assert(guest && width(guest) == 1280 && height(guest) == 720);
    assert(android_native_window_compat_horizon_handle(guest, &actual_window));
    assert(actual_window == &horizon_window);
    assert(!morrow_jni_runtime_bind_horizon_surface(&other_window, 1280, 720));
    assert(morrow_jni_runtime_bind_horizon_surface(&horizon_window, 960, 540) == surface);
    assert(width(guest) == 960 && height(guest) == 540);
    assert(morrow_jni_runtime_unbind_surface());
    assert(!morrow_jni_runtime_surface_bound());
    assert(!android_native_window_compat_horizon_handle(guest, &actual_window));
    assert(actual_window == nullptr);
    release(guest); // Borrowed references survive until their own release.
    assert(morrow_jni_runtime_unbind_surface());
    puts("morrow_jni_runtime_test: JNI provider, thread identity and Surface passed");
    return 0;
}
