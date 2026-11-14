#include <string> // The pinned libjnivm headers need this before unordered_map.
#include <jnivm.h>

#include "android_jni_vm_bridge.h"
#include "android_asset_compat.h"
#include "android_native_window_compat.h"
#include "morrow_jni_runtime.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <unistd.h>

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
using AssetFromJava = void *(*)(void *, void *);
using AssetOpen = void *(*)(void *, const char *, int);
using AssetLength = off_t (*)(void *);
using AssetClose = void (*)(void *);

template<typename Callback> static Callback native_window_symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_native_window_compat_resolve(name, nullptr, nullptr, &address));
    Callback callback = nullptr;
    static_assert(sizeof(callback) == sizeof(address), "pointer width mismatch");
    std::memcpy(&callback, &address, sizeof(callback));
    return callback;
}

template<typename Callback> static Callback asset_symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_asset_compat_resolve(name, nullptr, nullptr, &address));
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
    assert(!morrow_jni_runtime_game_activity_ready());
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

    void *activity = morrow_jni_runtime_game_activity();
    assert(activity && morrow_jni_runtime_game_activity_ready());
    assert(morrow_jni_runtime_game_activity() == activity);
    jclass activity_class = main_env->FindClass("com/google/androidgamesdk/GameActivity");
    jclass insets_class = main_env->FindClass("androidx/core/graphics/Insets");
    assert(activity_class && insets_class);
    assert(main_env->IsInstanceOf(static_cast<jobject>(activity), activity_class));
    jmethodID method = main_env->GetMethodID(activity_class, "getWaterfallInsets",
                                              "()Landroidx/core/graphics/Insets;");
    assert(method);
    jmethodID assets_method = main_env->GetMethodID(activity_class, "getAssets",
                                        "()Landroid/content/res/AssetManager;");
    assert(assets_method);
    // Without an existing asset directory the Activity cannot invent one.
    assert(!main_env->CallObjectMethod(static_cast<jobject>(activity), assets_method));
    jobject insets = main_env->CallObjectMethod(static_cast<jobject>(activity), method);
    assert(insets && main_env->IsInstanceOf(insets, insets_class));
    for (const char *field : {"left", "top", "right", "bottom"}) {
        jfieldID id = main_env->GetFieldID(insets_class, field, "I");
        assert(id && main_env->GetIntField(insets, id) == 0);
    }
    main_env->DeleteLocalRef(insets);

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
    jobject surface_local_alias = main_env->NewLocalRef(static_cast<jobject>(surface));
    jobject surface_global_alias = main_env->NewGlobalRef(static_cast<jobject>(surface));
    assert(surface_local_alias && surface_global_alias);
    assert(surface_local_alias != static_cast<jobject>(surface));
    assert(surface_global_alias != static_cast<jobject>(surface));
    assert(main_env->IsSameObject(surface_local_alias, static_cast<jobject>(surface)));
    assert(main_env->IsSameObject(surface_global_alias, static_cast<jobject>(surface)));
    void *local_guest = from_surface(main_env, surface_local_alias);
    void *global_guest = from_surface(main_env, surface_global_alias);
    assert(local_guest == guest && global_guest == guest);
    assert(!morrow_jni_runtime_bind_horizon_surface(&other_window, 1280, 720));
    assert(morrow_jni_runtime_bind_horizon_surface(&horizon_window, 960, 540) == surface);
    assert(width(guest) == 960 && height(guest) == 540);
    assert(morrow_jni_runtime_unbind_surface());
    assert(!morrow_jni_runtime_surface_bound());
    assert(!from_surface(main_env, surface_local_alias));
    assert(!from_surface(main_env, surface_global_alias));
    assert(!android_native_window_compat_horizon_handle(guest, &actual_window));
    assert(actual_window == nullptr);
    release(local_guest);
    release(global_guest);
    release(guest); // Borrowed references survive until their own release.
    main_env->DeleteLocalRef(surface_local_alias);
    main_env->DeleteGlobalRef(surface_global_alias);
    assert(morrow_jni_runtime_unbind_surface());

    auto from_java = asset_symbol<AssetFromJava>("AAssetManager_fromJava");
    auto open_asset = asset_symbol<AssetOpen>("AAssetManager_open");
    auto asset_length = asset_symbol<AssetLength>("AAsset_getLength");
    auto close_asset = asset_symbol<AssetClose>("AAsset_close");
    assert(!morrow_jni_runtime_asset_manager_bound());
    assert(!morrow_jni_runtime_bind_asset_manager("/missing-asset-directory"));
    char root[] = "/tmp/morrow-jni-assets-XXXXXX";
    assert(mkdtemp(root));
    char asset_path[128];
    assert(snprintf(asset_path, sizeof(asset_path), "%s/test.asset", root) > 0);
    FILE *file = fopen(asset_path, "wb");
    assert(file && fwrite("hello!", 1, 6, file) == 6 && fclose(file) == 0);
    void *asset_java = morrow_jni_runtime_bind_asset_manager(root);
    assert(asset_java && morrow_jni_runtime_asset_manager_bound());
    assert(main_env->IsInstanceOf(static_cast<jobject>(asset_java),
                                  main_env->FindClass("android/content/res/AssetManager")));
    assert(from_java(main_env, &horizon_window) == nullptr);
    void *native_manager = from_java(main_env, asset_java);
    assert(native_manager);
    jobject activity_assets = main_env->CallObjectMethod(
        static_cast<jobject>(activity), assets_method);
    assert(activity_assets && main_env->IsSameObject(activity_assets,
                                                    static_cast<jobject>(asset_java)));
    assert(from_java(main_env, activity_assets) == native_manager);
    jobject alias = main_env->NewLocalRef(static_cast<jobject>(asset_java));
    assert(alias && main_env->IsSameObject(alias, static_cast<jobject>(asset_java)));
    assert(from_java(main_env, alias) == native_manager);
    main_env->DeleteLocalRef(alias);
    assert(morrow_jni_runtime_bind_asset_manager(root) == asset_java);
    assert(!morrow_jni_runtime_bind_asset_manager("/tmp"));
    assert(!open_asset(native_manager, "../test.asset", 0));
    void *asset = open_asset(native_manager, "test.asset", 0);
    assert(asset && asset_length(asset) == 6);
    assert(morrow_jni_runtime_unbind_asset_manager());
    assert(!morrow_jni_runtime_asset_manager_bound());
    assert(!from_java(main_env, asset_java));
    assert(!from_java(main_env, activity_assets));
    assert(!main_env->CallObjectMethod(static_cast<jobject>(activity), assets_method));
    main_env->DeleteLocalRef(activity_assets);
    assert(!open_asset(native_manager, "test.asset", 0));
    close_asset(asset);
    assert(morrow_jni_runtime_unbind_asset_manager());
    assert(morrow_jni_runtime_release_game_activity());
    assert(!morrow_jni_runtime_game_activity_ready());
    assert(morrow_jni_runtime_release_game_activity());
    assert(unlink(asset_path) == 0 && rmdir(root) == 0);
    puts("morrow_jni_runtime_test: JNI, GameActivity assets/insets and aliases passed");
    return 0;
}
