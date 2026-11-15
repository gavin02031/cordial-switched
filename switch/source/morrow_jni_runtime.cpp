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

class HorizonClassLoader : public jnivm::Object {
public:
    std::shared_ptr<jnivm::Class> loadClass(
        jnivm::ENV *env, std::shared_ptr<jnivm::String> name)
    {
        if (!name)
            return nullptr;
        std::string path = *name;
        for (char &part : path)
            if (part == '.') part = '/';
        // The Android APK's classpath is not installed on Horizon. Resolve
        // only classes whose platform identities we have explicitly defined;
        // inventing a placeholder Class would hide the missing Android API.
        static const char *const known[] = {
            "java/lang/Object", "java/lang/Class", "java/lang/String",
            "java/lang/ClassLoader", "android/view/Surface",
            "android/content/res/AssetManager", "androidx/core/graphics/Insets",
            "com/google/androidgamesdk/GameActivity"
        };
        for (const char *entry : known)
            if (path == entry)
                return env->GetClass(entry);
        return nullptr;
    }

    static void Register(jnivm::ENV *env)
    {
        auto klass = env->GetClass<HorizonClassLoader>("java/lang/ClassLoader");
        klass->HookInstanceFunction(env, "loadClass", &HorizonClassLoader::loadClass);
        klass->HookInstanceFunction(env, "findClass", &HorizonClassLoader::loadClass);
    }
};

std::shared_ptr<HorizonClassLoader> class_loader_owner;

class HorizonGameActivity : public jnivm::Object {
public:
    MorrowGameActivityRequests requests = {};

    std::shared_ptr<HorizonClassLoader> getClassLoader(jnivm::ENV *)
    {
        std::lock_guard<std::mutex> lock(start_mutex);
        if (!class_loader_owner)
            class_loader_owner = std::make_shared<HorizonClassLoader>();
        return class_loader_owner;
    }

    void setWindowFlags(jnivm::ENV *, jint flags, jint mask)
    {
        // Horizon has no Android WindowManager. Preserve the request for
        // diagnostics rather than claiming these bits changed a window.
        std::lock_guard<std::mutex> lock(start_mutex);
        if (requests.window_flag_requests < UINT32_MAX)
            ++requests.window_flag_requests;
        requests.last_window_flags = flags;
        requests.last_window_mask = mask;
    }

    void setImeEditorInfoFields(jnivm::ENV *, jint first, jint second, jint third)
    {
        // No soft keyboard/EditorInfo exists here yet. The desktop bridge
        // declares this exact signature but cannot apply those fields either.
        std::lock_guard<std::mutex> lock(start_mutex);
        if (requests.ime_editor_info_requests < UINT32_MAX)
            ++requests.ime_editor_info_requests;
        requests.last_ime_value_1 = first;
        requests.last_ime_value_2 = second;
        requests.last_ime_value_3 = third;
    }

    std::shared_ptr<AssetManagerObject> getAssets(jnivm::ENV *)
    {
        std::lock_guard<std::mutex> lock(start_mutex);
        // No asset directory means no Android asset owner. Returning a fake
        // manager would turn an unresolved resource into a silent success.
        if (!asset_owner)
            return nullptr;
        return asset_owner;
    }

    std::shared_ptr<ZeroInsets> getWaterfallInsets(jnivm::ENV *)
    {
        // Horizon's fullscreen display has no Android status bar or cutout.
        // Zero is a real measurement of the host layout, not a stubbed value.
        // HookInstanceFunction converts the returned shared_ptr to exactly
        // one JNI local. Converting here as well leaked an unreachable second
        // local on every call from the engine's UI.
        return std::make_shared<ZeroInsets>();
    }

    static void Register(jnivm::ENV *env)
    {
        ZeroInsets::Register(env); // Needed for the method's JNI descriptor.
        HorizonClassLoader::Register(env);
        env->GetClass<HorizonSurface>("android/view/Surface");
        env->GetClass<AssetManagerObject>("android/content/res/AssetManager");
        env->GetClass<HorizonGameActivity>("com/google/androidgamesdk/GameActivity");
        auto klass = env->GetClass("com/google/androidgamesdk/GameActivity");
        klass->HookInstanceFunction(env, "getClassLoader",
                                    &HorizonGameActivity::getClassLoader);
        klass->HookInstanceFunction(env, "setWindowFlags",
                                    &HorizonGameActivity::setWindowFlags);
        klass->HookInstanceFunction(env, "setImeEditorInfoFields",
                                    &HorizonGameActivity::setImeEditorInfoFields);
        klass->HookInstanceFunction(env, "getAssets", &HorizonGameActivity::getAssets);
        klass->HookInstanceFunction(env, "getWaterfallInsets",
                                    &HorizonGameActivity::getWaterfallInsets);
    }
};

std::shared_ptr<HorizonGameActivity> game_activity_owner;
jobject game_activity_reference;

bool same_known_java_object(void *environment, void *candidate, void *bound)
{
    // Both NDK adapters also serve the non-JNI preview.
    // Do not give IsSameObject an arbitrary pointer. The pinned VM represents
    // references as Object*, so only objects in this thread's local frames or
    // the VM's global collection are safe to inspect.
    if (!attached_to_runtime || !runtime || !environment || !candidate || !bound)
        return false;
    JNIEnv *jni = runtime->GetJNIEnv();
    if (jni != environment)
        return false;
    jnivm::ENV *env = runtime->GetEnv().get();
    bool known = false;
    for (const auto &frame : env->localframe) {
        for (const auto &ref : frame)
            if (ref.get() == candidate) known = true;
    }
    if (!known) {
        std::lock_guard<std::mutex> lock(runtime->mtx);
        for (const auto &ref : runtime->globals)
            if (ref.get() == candidate) known = true;
    }
    return known && jni->IsSameObject(static_cast<jobject>(candidate),
                                      static_cast<jobject>(bound)) == JNI_TRUE;
}

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

uint32_t synthetic_native_calls;

jint synthetic_native_increment(JNIEnv *env, jclass klass, jint value)
{
    if (!env || !klass)
        return JNI_ERR;
    jclass expected = env->FindClass("dev/morrow/diagnostics/OnLoadProbe");
    if (!expected)
        return JNI_ERR;
    const bool correct_class = env->IsSameObject(klass, expected) == JNI_TRUE;
    env->DeleteLocalRef(expected);
    if (!correct_class)
        return JNI_ERR;
    ++synthetic_native_calls;
    return value + 7;
}

jint synthetic_jni_onload(JavaVM *vm, void *)
{
    // A controlled JNI_OnLoad-shaped entry, not an engine call. Exercise the
    // exact JavaVM* and RegisterNatives ABI an Android library will receive.
    JNIEnv *env = nullptr;
    if (!vm || vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK ||
        !env || env->GetVersion() != JNI_VERSION_1_6)
        return JNI_ERR;
    jclass klass = env->FindClass("dev/morrow/diagnostics/OnLoadProbe");
    if (!klass)
        return JNI_ERR;
    // The limited JNI VM needs a known method declaration before it can
    // attach a native callback without guessing instance/static semantics.
    if (!env->GetStaticMethodID(klass, "nativeIncrement", "(I)I")) {
        env->DeleteLocalRef(klass);
        return JNI_ERR;
    }
    JNINativeMethod method = {
        const_cast<char *>("nativeIncrement"),
        const_cast<char *>("(I)I"),
        reinterpret_cast<void *>(&synthetic_native_increment)
    };
    const jint result = env->RegisterNatives(klass, &method, 1);
    env->DeleteLocalRef(klass);
    return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
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
        candidate->GetEnv()->GetClass("dev/morrow/diagnostics/OnLoadProbe");
        const AndroidJniVmProvider provider = {get_env, attach, detach, nullptr};
        if (!android_jni_vm_bridge_install_provider(&provider, candidate.get()))
            return false;
        // Constructing the VM attached this caller; other threads must
        // explicitly attach through the bridge before obtaining their env.
        attached_to_runtime = true;
        runtime = std::move(candidate);
        android_asset_compat_set_reference_matcher(same_known_java_object);
        android_native_window_compat_set_reference_matcher(same_known_java_object);
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

extern "C" void *morrow_jni_runtime_jni_env(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return runtime && attached_to_runtime ? runtime->GetJNIEnv() : nullptr;
}

extern "C" bool morrow_jni_runtime_probe_onload(MorrowJniOnLoadProbe *report)
{
    if (!report)
        return false;
    *report = {};
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime)
        return false;

    try {
        JavaVM *vm = runtime->GetJavaVM();
        JNIEnv *env = runtime->GetJNIEnv();
        report->vm_available = vm && env;
        if (!report->vm_available)
            return false;
        const size_t locals_before = runtime->GetEnv()->localframe.front().size();
        report->jni_onload_accepted = synthetic_jni_onload(vm, nullptr) == JNI_VERSION_1_6;
        jclass klass = nullptr;
        if (report->jni_onload_accepted) {
            klass = env->FindClass("dev/morrow/diagnostics/OnLoadProbe");
            if (klass) {
                jmethodID method = env->GetStaticMethodID(klass, "nativeIncrement", "(I)I");
                report->native_registered = method != nullptr;
                if (method) {
                    const uint32_t calls_before = synthetic_native_calls;
                    const jint value = env->CallStaticIntMethod(klass, method, 35);
                    report->native_invoked = value == 42 &&
                        synthetic_native_calls == calls_before + 1 && !env->ExceptionCheck();
                }
                report->native_unregistered = env->UnregisterNatives(klass) == JNI_OK;
                env->DeleteLocalRef(klass);
            }
        }
        report->local_references_released =
            runtime->GetEnv()->localframe.front().size() == locals_before;
        return report->jni_onload_accepted && report->native_registered &&
               report->native_invoked && report->native_unregistered &&
               report->local_references_released;
    } catch (...) {
        return false;
    }
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
        if (!global) {
            jni->DeleteLocalRef(local);
            return nullptr;
        }
        if (!android_native_window_compat_bind_horizon_surface(
                global, horizon_window, width, height)) {
            jni->DeleteGlobalRef(global);
            jni->DeleteLocalRef(local);
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
        if (!global) {
            jni->DeleteLocalRef(local);
            return nullptr;
        }
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
        if (!global) {
            jni->DeleteLocalRef(local);
            return nullptr;
        }
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

extern "C" bool morrow_jni_runtime_activity_requests(
    MorrowGameActivityRequests *requests)
{
    if (!requests)
        return false;
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!game_activity_owner)
        return false;
    *requests = game_activity_owner->requests;
    return true;
}
