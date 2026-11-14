// The pinned libjnivm VM is used here, rather than imitating Java objects.
#include <string> // Pinned vm.h requires std::hash<string> before unordered_map.
#include <jnivm.h>

#include "morrow_jni_runtime.h"
#include "android_jni_vm_bridge.h"
#include "android_asset_compat.h"
#include "android_native_window_compat.h"

#include <memory>
#include <mutex>

namespace {

std::mutex start_mutex;
std::unique_ptr<jnivm::VM> runtime;
thread_local bool attached_to_runtime;

class HorizonSurface : public jnivm::Object {};
std::shared_ptr<HorizonSurface> surface_owner;
jobject surface_reference;
void *surface_window;

class AssetManagerObject : public jnivm::Object {};
std::shared_ptr<AssetManagerObject> asset_owner;
jobject asset_reference;
std::string asset_directory;

class ZeroInsets : public jnivm::Object {
public:
    jint left = 0, top = 0, right = 0, bottom = 0;

    static void Register(jnivm::ENV *env)
    {
        env->GetClass<ZeroInsets>("androidx/core/graphics/Insets");
        auto klass = env->GetClass("androidx/core/graphics/Insets");
        klass->HookInstance(env, "left", &ZeroInsets::left);
        klass->HookInstance(env, "top", &ZeroInsets::top);
        klass->HookInstance(env, "right", &ZeroInsets::right);
        klass->HookInstance(env, "bottom", &ZeroInsets::bottom);
    }
};

class HorizonGameActivity : public jnivm::Object {
public:
    std::shared_ptr<ZeroInsets> getWaterfallInsets(jnivm::ENV *env)
    {
        // Horizon's fullscreen display has no Android status bar or cutout.
        // Zero is a real measurement of the host layout, not a stubbed value.
        auto value = std::make_shared<ZeroInsets>();
        jnivm::JNITypes<std::shared_ptr<ZeroInsets>>::ToJNIType(env, value);
        return value;
    }

    static void Register(jnivm::ENV *env)
    {
        ZeroInsets::Register(env); // Needed for the method's JNI descriptor.
        env->GetClass<HorizonGameActivity>("com/google/androidgamesdk/GameActivity");
        auto klass = env->GetClass("com/google/androidgamesdk/GameActivity");
        klass->HookInstanceFunction(env, "getWaterfallInsets",
                                    &HorizonGameActivity::getWaterfallInsets);
    }
};

std::shared_ptr<HorizonGameActivity> game_activity_owner;
jobject game_activity_reference;

int32_t get_env(void *context, void **environment, int32_t version)
{
    if (!attached_to_runtime)
        return ANDROID_JNI_EDETACHED;
    if (version != ANDROID_JNI_VERSION_1_6)
        return ANDROID_JNI_EVERSION;
    auto *vm = static_cast<jnivm::VM *>(context);
    // The pinned VM's GetEnv attaches unknown threads. Never expose that
    // behaviour through the strict bridge: JNI requires JNI_EDETACHED first.
    return vm->GetJavaVM()->GetEnv(environment, version);
}

int32_t attach(void *context, void **environment, void *arguments, bool daemon)
{
    auto *vm = static_cast<jnivm::VM *>(context);
    JavaVM *java_vm = vm->GetJavaVM();
    JNIEnv *env = nullptr;
    const jint result = daemon
        ? java_vm->AttachCurrentThreadAsDaemon(&env, arguments)
        : java_vm->AttachCurrentThread(&env, arguments);
    *environment = env;
    if (result == JNI_OK && env)
        attached_to_runtime = true;
    return result;
}

int32_t detach(void *context)
{
    if (!attached_to_runtime)
        return ANDROID_JNI_EDETACHED;
    auto *vm = static_cast<jnivm::VM *>(context);
    const jint result = vm->GetJavaVM()->DetachCurrentThread();
    if (result == JNI_OK)
        attached_to_runtime = false;
    return result;
}

} // namespace

extern "C" bool morrow_jni_runtime_start(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (runtime)
        return true;
    try {
        std::unique_ptr<jnivm::VM> candidate(new jnivm::VM());
        JNIEnv *env = candidate->GetJNIEnv();
        if (!env || !env->functions || env->GetVersion() != JNI_VERSION_1_6)
            return false;
        HorizonGameActivity::Register(candidate->GetEnv().get());
        const AndroidJniVmProvider provider = {get_env, attach, detach, nullptr};
        if (!android_jni_vm_bridge_install_provider(&provider, candidate.get()))
            return false;
        // Constructing the VM attached this caller; other threads must
        // explicitly attach through the bridge before obtaining their env.
        attached_to_runtime = true;
        runtime = std::move(candidate);
        return true;
    } catch (...) {
        // An allocation or JNI exception cannot cross the C entry point.
        return false;
    }
}

extern "C" void *morrow_jni_runtime_java_vm(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return runtime ? runtime->GetJavaVM() : nullptr;
}

extern "C" void *morrow_jni_runtime_bind_horizon_surface(void *horizon_window,
                                                          int32_t width, int32_t height)
{
    if (!horizon_window || width <= 0 || height <= 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime)
        return nullptr;
    if (surface_reference) {
        // A resize keeps Java identity. Switching displays requires an
        // explicit unbind, so a borrowed guest ANativeWindow never wanders.
        if (surface_window != horizon_window ||
            !android_native_window_compat_bind_horizon_surface(
                surface_reference, horizon_window, width, height))
            return nullptr;
        return surface_reference;
    }
    try {
        jnivm::ENV *env = runtime->GetEnv().get();
        env->GetClass<HorizonSurface>("android/view/Surface");
        auto owner = std::make_shared<HorizonSurface>();
        JNIEnv *jni = env->GetJNIEnv();
        jobject local = jnivm::JNITypes<std::shared_ptr<HorizonSurface>>::ToJNIType(
            env, owner);
        jobject global = jni->NewGlobalRef(local);
        if (!global)
            return nullptr;
        if (!android_native_window_compat_bind_horizon_surface(
                global, horizon_window, width, height)) {
            jni->DeleteGlobalRef(global);
            return nullptr;
        }
        // libjnivm's NewGlobalRef wraps an Object* in a distinct JNI object.
        // Register the reference we will hand to the host, not the raw C++
        // object address. A different ref must be explicitly recognised later.
        surface_owner = std::move(owner);
        surface_reference = global;
        surface_window = horizon_window;
        jni->DeleteLocalRef(local);
        return global;
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool morrow_jni_runtime_surface_bound(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return surface_reference != nullptr;
}

extern "C" bool morrow_jni_runtime_unbind_surface(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!surface_reference)
        return true;
    if (!runtime || !attached_to_runtime)
        return false;
    android_native_window_compat_unbind_surface(surface_reference);
    runtime->GetJNIEnv()->DeleteGlobalRef(surface_reference);
    surface_reference = nullptr;
    surface_window = nullptr;
    surface_owner.reset();
    return true;
}

extern "C" void *morrow_jni_runtime_bind_asset_manager(const char *asset_root)
{
    if (!asset_root || !*asset_root)
        return nullptr;
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime)
        return nullptr;
    if (asset_reference) {
        // A root change must not quietly redirect an existing Java object.
        // A guest may still hold its old AAssetManager handle.
        if (asset_directory != asset_root ||
            !android_asset_compat_bind_manager(asset_reference, asset_root))
            return nullptr;
        return asset_reference;
    }
    try {
        std::string chosen_root(asset_root);
        jnivm::ENV *env = runtime->GetEnv().get();
        env->GetClass<AssetManagerObject>("android/content/res/AssetManager");
        auto owner = std::make_shared<AssetManagerObject>();
        JNIEnv *jni = env->GetJNIEnv();
        jobject local = jnivm::JNITypes<std::shared_ptr<AssetManagerObject>>::ToJNIType(
            env, owner);
        jobject global = jni->NewGlobalRef(local);
        if (!global)
            return nullptr;
        if (!android_asset_compat_bind_manager(global, asset_root)) {
            jni->DeleteGlobalRef(global);
            jni->DeleteLocalRef(local);
            return nullptr;
        }
        asset_directory.swap(chosen_root);
        asset_owner = std::move(owner);
        asset_reference = global;
        jni->DeleteLocalRef(local);
        return global;
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool morrow_jni_runtime_asset_manager_bound(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return asset_reference != nullptr;
}

extern "C" bool morrow_jni_runtime_unbind_asset_manager(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!asset_reference)
        return true;
    if (!runtime || !attached_to_runtime)
        return false;
    android_asset_compat_unbind_manager(asset_reference);
    runtime->GetJNIEnv()->DeleteGlobalRef(asset_reference);
    asset_reference = nullptr;
    asset_directory.clear();
    asset_owner.reset();
    return true;
}

extern "C" void *morrow_jni_runtime_game_activity(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime)
        return nullptr;
    if (game_activity_reference)
        return game_activity_reference;
    try {
        jnivm::ENV *env = runtime->GetEnv().get();
        auto owner = std::make_shared<HorizonGameActivity>();
        JNIEnv *jni = env->GetJNIEnv();
        jobject local = jnivm::JNITypes<std::shared_ptr<HorizonGameActivity>>::ToJNIType(
            env, owner);
        jobject global = jni->NewGlobalRef(local);
        if (!global)
            return nullptr;
        game_activity_owner = std::move(owner);
        game_activity_reference = global;
        jni->DeleteLocalRef(local);
        return global;
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool morrow_jni_runtime_game_activity_ready(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return game_activity_reference != nullptr;
}

extern "C" bool morrow_jni_runtime_release_game_activity(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!game_activity_reference)
        return true;
    if (!runtime || !attached_to_runtime)
        return false;
    runtime->GetJNIEnv()->DeleteGlobalRef(game_activity_reference);
    game_activity_reference = nullptr;
    game_activity_owner.reset();
    return true;
}
