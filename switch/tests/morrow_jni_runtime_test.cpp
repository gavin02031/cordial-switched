#include <string> // The pinned libjnivm headers need this before unordered_map.
#include <jnivm.h>

#include "android_jni_vm_bridge.h"
#include "android_asset_compat.h"
#include "android_configuration_compat.h"
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
    void *activity;
    jmethodID set_window_flags;
};

using FromSurface = void *(*)(void *, void *);
using WindowDimension = int32_t (*)(void *);
using ReleaseWindow = void (*)(void *);
using AssetFromJava = void *(*)(void *, void *);
using AssetOpen = void *(*)(void *, const char *, int);
using AssetLength = off_t (*)(void *);
using AssetClose = void (*)(void *);
using NewConfiguration = void *(*)(void);
using DeleteConfiguration = void (*)(void *);
using FromAssetManager = void (*)(void *, void *);
using ConfigurationGetInteger = int32_t (*)(void *);
using ConfigurationGetLocale = void (*)(void *, char *);

static jobject expected_instance;
static jobject expected_activity;
static jobject expected_assets;
static jobject expected_configuration;
static const char *expected_path;
static unsigned native_activity_calls;
static jclass expected_bridge_class;
static jobject expected_bridge_init_params, expected_bridge_start_params;
static jobject expected_bridge_surface, expected_bridge_platform;
static unsigned native_bridge_calls[6];

static void synthetic_bridge_global(JNIEnv *env, jclass klass)
{
    assert(env && env->IsSameObject(klass, expected_bridge_class));
    ++native_bridge_calls[0];
}
static void synthetic_bridge_update_adapter(JNIEnv *env, jclass klass)
{
    assert(env && env->IsSameObject(klass, expected_bridge_class));
    ++native_bridge_calls[1];
}
static void synthetic_bridge_init(JNIEnv *env, jclass klass, jobject params)
{
    assert(env && env->IsSameObject(klass, expected_bridge_class));
    assert(env->IsSameObject(params, expected_bridge_init_params));
    ++native_bridge_calls[2];
}
static void synthetic_bridge_lua(JNIEnv *env, jclass klass)
{
    assert(env && env->IsSameObject(klass, expected_bridge_class));
    ++native_bridge_calls[3];
}
static void synthetic_bridge_surface(JNIEnv *env, jclass klass,
                                     jobject surface, jobject platform)
{
    assert(env && env->IsSameObject(klass, expected_bridge_class));
    assert(env->IsSameObject(surface, expected_bridge_surface));
    assert(env->IsSameObject(platform, expected_bridge_platform));
    ++native_bridge_calls[4];
}
static void synthetic_bridge_start(JNIEnv *env, jclass klass, jobject params)
{
    assert(env && env->IsSameObject(klass, expected_bridge_class));
    assert(env->IsSameObject(params, expected_bridge_start_params));
    ++native_bridge_calls[5];
}

static jlong synthetic_game_activity_init(JNIEnv *env, jobject activity,
                                          jstring internal, jstring obb,
                                          jstring external, jobject assets,
                                          jbyteArray saved_state, jobject configuration)
{
    assert(env && expected_path && !saved_state);
    assert(env->IsSameObject(activity, expected_activity));
    assert(env->IsSameObject(assets, expected_assets));
    assert(env->IsSameObject(configuration, expected_configuration));
    for (jstring path : {internal, obb, external}) {
        assert(path);
        const char *characters = env->GetStringUTFChars(path, nullptr);
        assert(characters && !strcmp(characters, expected_path));
        env->ReleaseStringUTFChars(path, characters);
    }
    ++native_activity_calls;
    return 0x12345;
}
static jint instance_increment(JNIEnv *env, jobject instance, jint value)
{
    assert(env->IsSameObject(instance, expected_instance));
    return value + 1;
}

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

template<typename Callback> static Callback configuration_symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_configuration_compat_resolve(name, nullptr, nullptr, &address));
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
    assert(!morrow_jni_runtime_jni_env());
    assert(f->attach_daemon(worker->bridge, &env, nullptr) == ANDROID_JNI_OK);
    worker->worker_env = static_cast<JNIEnv *>(env);
    assert(worker->worker_env && worker->worker_env != worker->main_env);
    assert(morrow_jni_runtime_jni_env() == worker->worker_env);
    JavaVM *returned_vm = nullptr;
    assert(worker->worker_env->GetJavaVM(&returned_vm) == JNI_OK);
    assert(returned_vm == worker->native_vm);
    worker->worker_env->CallVoidMethod(static_cast<jobject>(worker->activity),
                                      worker->set_window_flags, 0x800, 0x400);
    assert(!worker->worker_env->ExceptionCheck());
    assert(f->get_env(worker->bridge, &env, ANDROID_JNI_VERSION_1_6) == ANDROID_JNI_OK);
    assert(env == worker->worker_env);
    assert(f->detach(worker->bridge) == ANDROID_JNI_OK);
    assert(f->get_env(worker->bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_EDETACHED && !env);
    assert(!morrow_jni_runtime_jni_env());
    assert(f->detach(worker->bridge) == ANDROID_JNI_EDETACHED);
    return nullptr;
}

int main()
{
    auto *bridge = reinterpret_cast<BridgeVm *>(android_jni_vm_bridge_instance());
    assert(bridge && bridge->functions);
    assert(!android_jni_vm_bridge_has_provider());
    assert(morrow_jni_runtime_java_vm() == nullptr);
    assert(morrow_jni_runtime_jni_env() == nullptr);
    assert(!morrow_jni_runtime_game_activity_ready());
    assert(!morrow_jni_runtime_app_bridge_start_params());
    assert(!morrow_jni_runtime_app_bridge_start_params_bound());
    assert(morrow_jni_runtime_release_app_bridge_start_params());
    assert(!morrow_jni_runtime_app_bridge_init_params());
    assert(!morrow_jni_runtime_app_bridge_init_params_bound());
    assert(morrow_jni_runtime_release_app_bridge_init_params());
    assert(!morrow_jni_runtime_configuration_bound());
    assert(!morrow_jni_runtime_configuration());
    int64_t measured_bytes = -1;
    assert(!morrow_jni_runtime_storage_available_bytes(&measured_bytes));
    assert(!morrow_jni_runtime_storage_available_bytes(nullptr));
    assert(!morrow_jni_runtime_local_storage_manager());
    assert(!morrow_jni_runtime_bind_storage_available_bytes(-1));
    // The synthetic measurement is provided by the host test, not a guessed
    // Android disk size. The Switch path uses fsFsGetFreeSpace instead.
    const int64_t available_bytes = INT64_C(3) * 1024 * 1024 * 1024;
    assert(morrow_jni_runtime_bind_storage_available_bytes(available_bytes));
    assert(morrow_jni_runtime_storage_available_bytes(&measured_bytes));
    assert(measured_bytes == available_bytes);
    assert(!morrow_jni_runtime_system_locale_bound());
    assert(!morrow_jni_runtime_bind_system_locale("en-US"));
    MorrowJniOnLoadProbe onload = {};
    assert(!morrow_jni_runtime_probe_onload(&onload));
    assert(!onload.vm_available);
    assert(!morrow_jni_runtime_probe_onload(nullptr));
    MorrowGameActivityRequests activity_requests = {};
    assert(!morrow_jni_runtime_activity_requests(&activity_requests));
    assert(!morrow_jni_runtime_activity_requests(nullptr));
    assert(morrow_jni_runtime_start());
    assert(morrow_jni_runtime_start());
    assert(!morrow_jni_runtime_bind_storage_available_bytes(available_bytes));
    for (const char *invalid : {"", "en_US", "en-US-", "en-US-extra",
                                "english-US", "en-1234", "en-??", "zh-Hans-INVALID"})
        assert(!morrow_jni_runtime_bind_system_locale(invalid));
    assert(morrow_jni_runtime_bind_system_locale("es-419"));
    assert(morrow_jni_runtime_bind_system_locale("es-419"));
    assert(!morrow_jni_runtime_bind_system_locale("en-US"));
    assert(morrow_jni_runtime_system_locale_bound());
    assert(android_jni_vm_bridge_has_provider());
    auto *native_vm = static_cast<JavaVM *>(morrow_jni_runtime_java_vm());
    assert(native_vm);
    void *env = nullptr;
    assert(bridge->functions->get_env(bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_OK);
    auto *main_env = static_cast<JNIEnv *>(env);
    assert(main_env && main_env->GetVersion() == JNI_VERSION_1_6);
    assert(morrow_jni_runtime_jni_env() == main_env);
    void *storage = morrow_jni_runtime_local_storage_manager();
    assert(storage && morrow_jni_runtime_local_storage_manager() == storage);
    jclass storage_class = main_env->FindClass("com/roblox/client/LocalStorageManager");
    assert(storage_class && main_env->IsInstanceOf(static_cast<jobject>(storage),
                                                   storage_class));
    jmethodID allocatable = main_env->GetMethodID(
        storage_class, "getAllocatableBytes", "()J");
    assert(allocatable);
    jnivm::ENV *storage_env = jnivm::ENV::FromJNIEnv(main_env);
    const size_t space_locals_before = storage_env->localframe.front().size();
    for (unsigned i = 0; i < 64; ++i) {
        assert(main_env->CallLongMethod(static_cast<jobject>(storage), allocatable) ==
               available_bytes);
        assert(!main_env->ExceptionCheck());
    }
    assert(storage_env->localframe.front().size() == space_locals_before);
    jobject storage_alias = main_env->NewLocalRef(static_cast<jobject>(storage));
    assert(storage_alias && main_env->IsSameObject(storage_alias,
                                                   static_cast<jobject>(storage)));
    assert(morrow_jni_runtime_release_local_storage_manager());
    assert(morrow_jni_runtime_release_local_storage_manager());
    assert(main_env->CallLongMethod(storage_alias, allocatable) == available_bytes);
    main_env->DeleteLocalRef(storage_alias);
    main_env->DeleteLocalRef(storage_class);
    JavaVM *returned_vm = nullptr;
    assert(main_env->GetJavaVM(&returned_vm) == JNI_OK);
    assert(returned_vm == native_vm);
    // Registered JNI (I)I callbacks must execute via CallStaticIntMethod,
    // with their declaring class and calling thread's JNIEnv passed through.
    for (unsigned i = 0; i < 64; ++i) {
        const bool passed = morrow_jni_runtime_probe_onload(&onload);
        if (!passed)
            fprintf(stderr, "JNI_OnLoad probe %u failed: vm=%d entry=%d registered=%d "
                    "called=%d unregistered=%d locals=%d\n", i,
                    onload.vm_available, onload.jni_onload_accepted,
                    onload.native_registered, onload.native_invoked,
                    onload.native_unregistered, onload.local_references_released);
        assert(passed);
        assert(onload.vm_available && onload.jni_onload_accepted &&
               onload.native_registered && onload.native_invoked &&
               onload.native_unregistered && onload.local_references_released);
    }
    assert(bridge->functions->get_env(bridge, &env, 0x00010008) ==
           ANDROID_JNI_EVERSION && !env);

    void *activity = morrow_jni_runtime_game_activity();
    assert(activity && morrow_jni_runtime_game_activity_ready());
    assert(morrow_jni_runtime_game_activity() == activity);
    jclass activity_class = main_env->FindClass("com/google/androidgamesdk/GameActivity");
    jclass insets_class = main_env->FindClass("androidx/core/graphics/Insets");
    jclass config_class = main_env->FindClass("android/content/res/Configuration");
    assert(activity_class && insets_class && config_class);
    assert(main_env->IsInstanceOf(static_cast<jobject>(activity), activity_class));
    void *configuration = morrow_jni_runtime_configuration();
    assert(configuration && morrow_jni_runtime_configuration_bound());
    assert(morrow_jni_runtime_configuration() == configuration);
    assert(main_env->IsInstanceOf(static_cast<jobject>(configuration), config_class));
    jobject config_alias = main_env->NewLocalRef(static_cast<jobject>(configuration));
    assert(config_alias && main_env->IsSameObject(config_alias,
                                                  static_cast<jobject>(configuration)));
    jmethodID instance_method = main_env->GetMethodID(
        activity_class, "nativeIncrement", "(I)I");
    assert(instance_method);
    JNINativeMethod registered = {
        const_cast<char *>("nativeIncrement"), const_cast<char *>("(I)I"),
        reinterpret_cast<void *>(&instance_increment)
    };
    assert(main_env->RegisterNatives(activity_class, &registered, 1) == JNI_OK);
    expected_instance = static_cast<jobject>(activity);
    assert(main_env->CallIntMethod(expected_instance, instance_method, 41) == 42);
    assert(!main_env->ExceptionCheck());
    assert(main_env->UnregisterNatives(activity_class) == JNI_OK);
    expected_instance = nullptr;
    JNINativeMethod unsupported = {
        const_cast<char *>("nativeLong"), const_cast<char *>("(J)J"),
        reinterpret_cast<void *>(&instance_increment)
    };
    assert(main_env->RegisterNatives(activity_class, &unsupported, 1) == JNI_ERR);
    JNINativeMethod undeclared = {
        const_cast<char *>("nativeUndeclared"), const_cast<char *>("(I)I"),
        reinterpret_cast<void *>(&instance_increment)
    };
    assert(main_env->RegisterNatives(activity_class, &undeclared, 1) == JNI_ERR);
    jmethodID get_loader = main_env->GetMethodID(
        activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    assert(get_loader);
    jobject loader = main_env->CallObjectMethod(static_cast<jobject>(activity), get_loader);
    assert(loader && main_env->IsInstanceOf(
        loader, main_env->FindClass("java/lang/ClassLoader")));
    jobject loader_again = main_env->CallObjectMethod(
        static_cast<jobject>(activity), get_loader);
    assert(loader_again && main_env->IsSameObject(loader, loader_again));
    jclass loader_class = main_env->FindClass("java/lang/ClassLoader");
    jmethodID load_class = main_env->GetMethodID(
        loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jmethodID find_class = main_env->GetMethodID(
        loader_class, "findClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    assert(load_class && find_class);
    jstring known_name = main_env->NewStringUTF("com.google.androidgamesdk.GameActivity");
    jclass loaded = static_cast<jclass>(main_env->CallObjectMethod(
        loader, load_class, known_name));
    assert(loaded && main_env->IsSameObject(loaded, activity_class));
    jstring asset_name = main_env->NewStringUTF("android/content/res/AssetManager");
    jclass loaded_asset = static_cast<jclass>(main_env->CallObjectMethod(
        loader, find_class, asset_name));
    assert(loaded_asset && main_env->IsSameObject(
        loaded_asset, main_env->FindClass("android/content/res/AssetManager")));
    jstring configuration_name = main_env->NewStringUTF("android.content.res.Configuration");
    jclass loaded_configuration = static_cast<jclass>(main_env->CallObjectMethod(
        loader, load_class, configuration_name));
    assert(loaded_configuration && main_env->IsSameObject(loaded_configuration, config_class));
    jstring unsupported_name = main_env->NewStringUTF(
        "com.roblox.client.startup.MainGameActivity");
    assert(!main_env->CallObjectMethod(loader, load_class, unsupported_name));
    main_env->DeleteLocalRef(known_name);
    main_env->DeleteLocalRef(asset_name);
    main_env->DeleteLocalRef(unsupported_name);
    main_env->DeleteLocalRef(configuration_name);
    main_env->DeleteLocalRef(loaded);
    main_env->DeleteLocalRef(loaded_asset);
    main_env->DeleteLocalRef(loaded_configuration);
    main_env->DeleteLocalRef(loader_again);
    main_env->DeleteLocalRef(loader);
    assert(morrow_jni_runtime_activity_requests(&activity_requests));
    assert(activity_requests.window_flag_requests == 0);
    assert(activity_requests.ime_editor_info_requests == 0);
    jmethodID set_window_flags = main_env->GetMethodID(
        activity_class, "setWindowFlags", "(II)V");
    jmethodID set_ime_info = main_env->GetMethodID(
        activity_class, "setImeEditorInfoFields", "(III)V");
    assert(set_window_flags && set_ime_info);
    main_env->CallVoidMethod(static_cast<jobject>(activity), set_window_flags,
                             0x400, 0x200);
    main_env->CallVoidMethod(static_cast<jobject>(activity), set_ime_info,
                             17, 23, 42);
    assert(morrow_jni_runtime_activity_requests(&activity_requests));
    assert(activity_requests.window_flag_requests == 1);
    assert(activity_requests.last_window_flags == 0x400);
    assert(activity_requests.last_window_mask == 0x200);
    assert(activity_requests.ime_editor_info_requests == 1);
    assert(activity_requests.last_ime_value_1 == 17);
    assert(activity_requests.last_ime_value_2 == 23);
    assert(activity_requests.last_ime_value_3 == 42);
    jmethodID method = main_env->GetMethodID(activity_class, "getWaterfallInsets",
                                              "()Landroidx/core/graphics/Insets;");
    assert(method);
    jmethodID assets_method = main_env->GetMethodID(activity_class, "getAssets",
                                        "()Landroid/content/res/AssetManager;");
    assert(assets_method);
    jmethodID resources_method = main_env->GetMethodID(
        activity_class, "getResources", "()Landroid/content/res/Resources;");
    assert(resources_method);
    // Without an existing asset directory the Activity cannot invent one.
    assert(!main_env->CallObjectMethod(static_cast<jobject>(activity), assets_method));
    assert(!main_env->CallObjectMethod(static_cast<jobject>(activity), resources_method));
    MorrowGameActivityInitResult init_result = {};
    assert(!morrow_jni_runtime_call_game_activity(
        reinterpret_cast<uintptr_t>(&synthetic_game_activity_init),
        "/valid-directory-not-yet-bound", &init_result));
    assert(!init_result.entry_invoked);
    jobject insets = main_env->CallObjectMethod(static_cast<jobject>(activity), method);
    assert(insets && main_env->IsInstanceOf(insets, insets_class));
    for (const char *field : {"left", "top", "right", "bottom"}) {
        jfieldID id = main_env->GetFieldID(insets_class, field, "I");
        assert(id && main_env->GetIntField(insets, id) == 0);
    }
    main_env->DeleteLocalRef(insets);
    // The local JNI patch must release the GetObjectClass reference from
    // CallMethod and IsInstanceOf; deleting our result leaves no hidden locals.
    jnivm::ENV *real_env = jnivm::ENV::FromJNIEnv(main_env);
    const size_t locals_before = real_env->localframe.front().size();
    for (unsigned i = 0; i < 64; ++i) {
        jobject repeated = main_env->CallObjectMethod(static_cast<jobject>(activity), method);
        assert(repeated && main_env->IsInstanceOf(repeated, insets_class));
        main_env->DeleteLocalRef(repeated);
    }
    const size_t locals_after = real_env->localframe.front().size();
    assert(locals_after == locals_before);
    for (unsigned i = 0; i < 64; ++i) {
        jobject repeated_config = main_env->NewLocalRef(static_cast<jobject>(configuration));
        assert(repeated_config && main_env->IsInstanceOf(repeated_config, config_class));
        main_env->DeleteLocalRef(repeated_config);
    }
    assert(real_env->localframe.front().size() == locals_before);

    WorkerResult worker = {bridge, main_env, native_vm, nullptr,
                           activity, set_window_flags};
    pthread_t thread;
    assert(pthread_create(&thread, nullptr, worker_run, &worker) == 0);
    assert(pthread_join(thread, nullptr) == 0);
    assert(worker.worker_env && worker.worker_env != main_env);
    assert(bridge->functions->get_env(bridge, &env, ANDROID_JNI_VERSION_1_6) ==
           ANDROID_JNI_OK && env == main_env);
    assert(morrow_jni_runtime_activity_requests(&activity_requests));
    assert(activity_requests.window_flag_requests == 2);
    assert(activity_requests.last_window_flags == 0x800);
    assert(activity_requests.last_window_mask == 0x400);

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
    jclass resources_class = main_env->FindClass("android/content/res/Resources");
    jclass metrics_class = main_env->FindClass("android/util/DisplayMetrics");
    assert(resources_class && metrics_class);
    jmethodID get_display_metrics = main_env->GetMethodID(
        resources_class, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
    jmethodID get_resources_configuration = main_env->GetMethodID(
        resources_class, "getConfiguration", "()Landroid/content/res/Configuration;");
    assert(get_display_metrics && get_resources_configuration);
    jobject resources = main_env->CallObjectMethod(
        static_cast<jobject>(activity), resources_method);
    assert(resources && main_env->IsInstanceOf(resources, resources_class));
    jobject resources_again = main_env->CallObjectMethod(
        static_cast<jobject>(activity), resources_method);
    assert(resources_again && main_env->IsSameObject(resources, resources_again));
    main_env->DeleteLocalRef(resources_again);
    jobject resource_configuration = main_env->CallObjectMethod(
        resources, get_resources_configuration);
    assert(resource_configuration && main_env->IsSameObject(
        resource_configuration, static_cast<jobject>(configuration)));
    main_env->DeleteLocalRef(resource_configuration);
    jfieldID config_density = main_env->GetFieldID(config_class, "densityDpi", "I");
    jfieldID config_orientation = main_env->GetFieldID(config_class, "orientation", "I");
    jfieldID config_width = main_env->GetFieldID(config_class, "screenWidthDp", "I");
    jfieldID config_height = main_env->GetFieldID(config_class, "screenHeightDp", "I");
    jfieldID config_smallest = main_env->GetFieldID(
        config_class, "smallestScreenWidthDp", "I");
    jfieldID config_touch = main_env->GetFieldID(config_class, "touchscreen", "I");
    jfieldID config_font = main_env->GetFieldID(config_class, "fontScale", "F");
    assert(config_density && config_orientation && config_width && config_height &&
           config_smallest && config_touch && config_font);
    assert(main_env->GetIntField(config_alias, config_density) == 160);
    assert(main_env->GetIntField(config_alias, config_orientation) == 2);
    assert(main_env->GetIntField(config_alias, config_width) == 1280);
    assert(main_env->GetIntField(config_alias, config_height) == 720);
    assert(main_env->GetIntField(config_alias, config_smallest) == 720);
    assert(main_env->GetIntField(config_alias, config_touch) == 3);
    assert(main_env->GetFloatField(config_alias, config_font) == 1.0f);
    jfieldID width_pixels = main_env->GetFieldID(metrics_class, "widthPixels", "I");
    jfieldID height_pixels = main_env->GetFieldID(metrics_class, "heightPixels", "I");
    jfieldID density = main_env->GetFieldID(metrics_class, "density", "F");
    jfieldID density_dpi = main_env->GetFieldID(metrics_class, "densityDpi", "I");
    jfieldID xdpi = main_env->GetFieldID(metrics_class, "xdpi", "F");
    assert(width_pixels && height_pixels && density && density_dpi && xdpi);
    const size_t metrics_locals_before = real_env->localframe.front().size();
    for (unsigned i = 0; i < 64; ++i) {
        jobject metrics = main_env->CallObjectMethod(resources, get_display_metrics);
        assert(metrics && main_env->IsInstanceOf(metrics, metrics_class));
        assert(main_env->GetIntField(metrics, width_pixels) == 1280);
        assert(main_env->GetIntField(metrics, height_pixels) == 720);
        assert(main_env->GetFloatField(metrics, density) == 1.0f);
        assert(main_env->GetIntField(metrics, density_dpi) == 160);
        assert(main_env->GetFloatField(metrics, xdpi) == 0.0f);
        main_env->DeleteLocalRef(metrics);
    }
    assert(!main_env->ExceptionCheck());
    assert(real_env->localframe.front().size() == metrics_locals_before);
    // The real surface alone is insufficient for App Bridge: an asset
    // directory is mandatory, and a null Java receiver must not be made up.
    assert(!morrow_jni_runtime_app_bridge_start_params());
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
    jobject resized_metrics = main_env->CallObjectMethod(resources, get_display_metrics);
    assert(resized_metrics && main_env->GetIntField(resized_metrics, width_pixels) == 960);
    assert(main_env->GetIntField(resized_metrics, height_pixels) == 540);
    main_env->DeleteLocalRef(resized_metrics);
    assert(main_env->GetIntField(config_alias, config_width) == 960);
    assert(main_env->GetIntField(config_alias, config_height) == 540);
    assert(main_env->GetIntField(config_alias, config_smallest) == 540);
    assert(main_env->GetIntField(config_alias, config_orientation) == 2);
    assert(morrow_jni_runtime_bind_horizon_surface(&horizon_window, 540, 960) == surface);
    assert(width(guest) == 540 && height(guest) == 960);
    assert(main_env->GetIntField(config_alias, config_width) == 540);
    assert(main_env->GetIntField(config_alias, config_height) == 960);
    assert(main_env->GetIntField(config_alias, config_smallest) == 540);
    assert(main_env->GetIntField(config_alias, config_orientation) == 1);
    assert(morrow_jni_runtime_unbind_surface());
    assert(!morrow_jni_runtime_surface_bound());
    assert(!main_env->CallObjectMethod(resources, get_display_metrics));
    assert(!main_env->CallObjectMethod(resources, get_resources_configuration));
    assert(main_env->GetIntField(config_alias, config_density) == 0);
    assert(main_env->GetIntField(config_alias, config_orientation) == 0);
    assert(main_env->GetIntField(config_alias, config_width) == 0);
    assert(main_env->GetIntField(config_alias, config_height) == 0);
    assert(main_env->GetIntField(config_alias, config_smallest) == 0);
    assert(main_env->GetIntField(config_alias, config_touch) == 3);
    assert(main_env->GetFloatField(config_alias, config_font) == 1.0f);
    assert(!main_env->CallObjectMethod(static_cast<jobject>(activity), resources_method));
    main_env->DeleteLocalRef(resources);
    main_env->DeleteLocalRef(resources_class);
    main_env->DeleteLocalRef(metrics_class);
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
    assert(morrow_jni_runtime_asset_configuration_bound());
    void *configured_native_manager = from_java(main_env, asset_java);
    assert(configured_native_manager);
    auto make_ndk_config = configuration_symbol<NewConfiguration>("AConfiguration_new");
    auto delete_ndk_config = configuration_symbol<DeleteConfiguration>(
        "AConfiguration_delete");
    auto from_ndk_assets = configuration_symbol<FromAssetManager>(
        "AConfiguration_fromAssetManager");
    auto ndk_width = configuration_symbol<ConfigurationGetInteger>(
        "AConfiguration_getScreenWidthDp");
    auto ndk_height = configuration_symbol<ConfigurationGetInteger>(
        "AConfiguration_getScreenHeightDp");
    auto ndk_language = configuration_symbol<ConfigurationGetLocale>(
        "AConfiguration_getLanguage");
    auto ndk_country = configuration_symbol<ConfigurationGetLocale>(
        "AConfiguration_getCountry");
    void *ndk_snapshot = make_ndk_config();
    assert(ndk_snapshot);
    from_ndk_assets(ndk_snapshot, configured_native_manager);
    assert(ndk_width(ndk_snapshot) == 0 && ndk_height(ndk_snapshot) == 0);
    char ndk_locale[2] = {};
    ndk_language(ndk_snapshot, ndk_locale);
    assert(ndk_locale[0] == 'e' && ndk_locale[1] == 's');
    ndk_country(ndk_snapshot, ndk_locale);
    assert(ndk_locale[0] == 0 && ndk_locale[1] == 0);
    AndroidAssetConfiguration ndk_configuration = {};
    assert(android_asset_compat_get_configuration(configured_native_manager,
                                                   &ndk_configuration));
    assert(ndk_configuration.language[0] == 'e' &&
           ndk_configuration.language[1] == 's');
    assert(ndk_configuration.country[0] == 0 && ndk_configuration.country[1] == 0);
    assert(ndk_configuration.screen_width_dp == 0 &&
           ndk_configuration.screen_height_dp == 0);
    assert(ndk_configuration.screen_size == 0 && ndk_configuration.nav_hidden == 0);
    assert(!morrow_jni_runtime_app_bridge_start_params()); // No live surface.
    void *app_surface = morrow_jni_runtime_bind_horizon_surface(
        &horizon_window, 1280, 720);
    assert(app_surface);
    assert(android_asset_compat_get_configuration(configured_native_manager,
                                                   &ndk_configuration));
    assert(ndk_configuration.screen_width_dp == 1280 &&
           ndk_configuration.screen_height_dp == 720);
    from_ndk_assets(ndk_snapshot, configured_native_manager);
    assert(ndk_width(ndk_snapshot) == 1280 && ndk_height(ndk_snapshot) == 720);
    void *start_params = morrow_jni_runtime_app_bridge_start_params();
    assert(start_params && morrow_jni_runtime_app_bridge_start_params_bound());
    void *init_params = morrow_jni_runtime_app_bridge_init_params();
    assert(init_params && morrow_jni_runtime_app_bridge_init_params_bound());
    assert(morrow_jni_runtime_app_bridge_init_params() == init_params);
    assert(morrow_jni_runtime_app_bridge_start_params() == start_params);
    jclass start_class = main_env->FindClass(
        "com/roblox/engine/jni/autovalue/StartAppParams");
    jclass platform_class = main_env->FindClass(
        "com/roblox/engine/jni/model/PlatformParams");
    assert(start_class && platform_class && main_env->IsInstanceOf(
        static_cast<jobject>(start_params), start_class));
    jclass init_class = main_env->FindClass(
        "com/roblox/engine/jni/autovalue/InitParams");
    jclass device_class = main_env->FindClass(
        "com/roblox/engine/jni/model/DeviceParams");
    assert(init_class && device_class && main_env->IsInstanceOf(
        static_cast<jobject>(init_params), init_class));
    jfieldID init_platform = main_env->GetFieldID(
        init_class, "platformParams", "Lcom/roblox/engine/jni/model/PlatformParams;");
    jmethodID init_platform_method = main_env->GetMethodID(
        init_class, "platformParams", "()Lcom/roblox/engine/jni/model/PlatformParams;");
    jfieldID init_device = main_env->GetFieldID(
        init_class, "deviceParams", "Lcom/roblox/engine/jni/model/DeviceParams;");
    jmethodID init_device_method = main_env->GetMethodID(
        init_class, "deviceParams", "()Lcom/roblox/engine/jni/model/DeviceParams;");
    jmethodID init_url_method = main_env->GetMethodID(
        init_class, "baseURL", "()Ljava/lang/String;");
    jmethodID init_agent_method = main_env->GetMethodID(
        init_class, "userAgent", "()Ljava/lang/String;");
    assert(init_platform && init_platform_method && init_device && init_device_method &&
           init_url_method && init_agent_method);
    jfieldID surface_field = main_env->GetFieldID(
        start_class, "surface", "Landroid/view/Surface;");
    jmethodID surface_method = main_env->GetMethodID(
        start_class, "surface", "()Landroid/view/Surface;");
    jfieldID platform_field = main_env->GetFieldID(
        start_class, "platformParams", "Lcom/roblox/engine/jni/model/PlatformParams;");
    jmethodID platform_method = main_env->GetMethodID(
        start_class, "platformParams", "()Lcom/roblox/engine/jni/model/PlatformParams;");
    assert(surface_field && surface_method && platform_field && platform_method);
    const size_t bridge_locals_before = real_env->localframe.front().size();
    expected_bridge_class = main_env->FindClass(
        "com/roblox/engine/jni/NativeGLInterface");
    assert(expected_bridge_class);
    expected_bridge_init_params = static_cast<jobject>(init_params);
    expected_bridge_start_params = static_cast<jobject>(start_params);
    expected_bridge_surface = static_cast<jobject>(app_surface);
    expected_bridge_platform = main_env->GetObjectField(
        static_cast<jobject>(start_params), platform_field);
    assert(expected_bridge_platform);
    const size_t bridge_call_locals_before = real_env->localframe.front().size();
    const uintptr_t bridge_functions[] = {
        reinterpret_cast<uintptr_t>(&synthetic_bridge_global),
        reinterpret_cast<uintptr_t>(&synthetic_bridge_update_adapter),
        reinterpret_cast<uintptr_t>(&synthetic_bridge_init),
        reinterpret_cast<uintptr_t>(&synthetic_bridge_lua),
        reinterpret_cast<uintptr_t>(&synthetic_bridge_surface),
        reinterpret_cast<uintptr_t>(&synthetic_bridge_start)
    };
    for (unsigned trial = 0; trial < 32; ++trial) {
        for (unsigned step = 0; step < 6; ++step) {
            MorrowAppBridgeCallResult call = {};
            assert(morrow_jni_runtime_call_app_bridge(bridge_functions[step],
                static_cast<MorrowAppBridgeStep>(step), &call));
            assert(call.entry_invoked && call.entry_returned &&
                   !call.java_exception_pending && !call.cpp_exception_caught);
        }
    }
    for (unsigned step = 0; step < 6; ++step)
        assert(native_bridge_calls[step] == 32);
    assert(real_env->localframe.front().size() == bridge_call_locals_before);
    main_env->DeleteLocalRef(expected_bridge_platform);
    expected_bridge_platform = expected_bridge_surface = nullptr;
    main_env->DeleteLocalRef(expected_bridge_class);
    expected_bridge_class = nullptr;
    expected_bridge_init_params = expected_bridge_start_params = nullptr;
    // The existing AutoValue getter test below uses the same local-frame
    // baseline and must still pass after 192 separate native boundary calls.
    const size_t bridge_after_calls = real_env->localframe.front().size();
    assert(bridge_after_calls == bridge_locals_before);
    for (unsigned i = 0; i < 32; ++i) {
        jobject field_surface = main_env->GetObjectField(
            static_cast<jobject>(start_params), surface_field);
        jobject method_surface = main_env->CallObjectMethod(
            static_cast<jobject>(start_params), surface_method);
        jobject field_platform = main_env->GetObjectField(
            static_cast<jobject>(start_params), platform_field);
        jobject method_platform = main_env->CallObjectMethod(
            static_cast<jobject>(start_params), platform_method);
        assert(field_surface && method_surface && field_platform && method_platform);
        assert(main_env->IsSameObject(field_surface, static_cast<jobject>(app_surface)));
        assert(main_env->IsSameObject(method_surface, static_cast<jobject>(app_surface)));
        assert(main_env->IsInstanceOf(field_platform, platform_class));
        assert(main_env->IsSameObject(field_platform, method_platform));
        jobject init_platform_ref = main_env->GetObjectField(
            static_cast<jobject>(init_params), init_platform);
        jobject init_platform_getter = main_env->CallObjectMethod(
            static_cast<jobject>(init_params), init_platform_method);
        assert(init_platform_ref && init_platform_getter);
        assert(main_env->IsSameObject(init_platform_ref, field_platform));
        assert(main_env->IsSameObject(init_platform_getter, field_platform));
        jobject device_field = main_env->GetObjectField(
            static_cast<jobject>(init_params), init_device);
        jobject device_getter = main_env->CallObjectMethod(
            static_cast<jobject>(init_params), init_device_method);
        assert(device_field && device_getter && main_env->IsSameObject(
            device_field, device_getter));
        assert(main_env->IsInstanceOf(device_field, device_class));
        jfieldID device_width = main_env->GetFieldID(
            device_class, "displayPhysicalWidthPixels", "I");
        jfieldID device_os = main_env->GetFieldID(
            device_class, "osVersion", "Ljava/lang/String;");
        jfieldID memory = main_env->GetFieldID(
            device_class, "deviceTotalMemoryMB", "I");
        assert(device_width && device_os && memory);
        assert(main_env->GetIntField(device_field, device_width) == 1280);
        assert(main_env->GetIntField(device_field, memory) == 0);
        assert(!main_env->GetObjectField(device_field, device_os));
        jstring base_url = static_cast<jstring>(main_env->CallObjectMethod(
            static_cast<jobject>(init_params), init_url_method));
        assert(base_url);
        const char *url_text = main_env->GetStringUTFChars(base_url, nullptr);
        assert(url_text && !strcmp(url_text, "https://www.roblox.com/"));
        main_env->ReleaseStringUTFChars(base_url, url_text);
        assert(!main_env->CallObjectMethod(static_cast<jobject>(init_params),
                                           init_agent_method));
        main_env->DeleteLocalRef(base_url);
        main_env->DeleteLocalRef(device_getter);
        main_env->DeleteLocalRef(device_field);
        main_env->DeleteLocalRef(init_platform_ref);
        main_env->DeleteLocalRef(init_platform_getter);
        void *window = from_surface(main_env, method_surface);
        assert(window && width(window) == 1280 && height(window) == 720);
        release(window);
        jfieldID touch = main_env->GetFieldID(platform_class, "isTouchDevice", "Z");
        jfieldID mm = main_env->GetFieldID(platform_class, "viewportWidthMm", "I");
        jfieldID path = main_env->GetFieldID(
            platform_class, "assetFolderPath", "Ljava/lang/String;");
        assert(touch && mm && path);
        assert(main_env->GetBooleanField(field_platform, touch) == JNI_TRUE);
        assert(main_env->GetIntField(field_platform, mm) == 0);
        jobject path_ref = main_env->GetObjectField(field_platform, path);
        assert(path_ref);
        const char *path_chars = main_env->GetStringUTFChars(
            static_cast<jstring>(path_ref), nullptr);
        assert(path_chars && !strcmp(path_chars, root));
        main_env->ReleaseStringUTFChars(static_cast<jstring>(path_ref), path_chars);
        main_env->DeleteLocalRef(path_ref);
        main_env->DeleteLocalRef(field_surface);
        main_env->DeleteLocalRef(method_surface);
        main_env->DeleteLocalRef(field_platform);
        main_env->DeleteLocalRef(method_platform);
    }
    assert(!main_env->ExceptionCheck());
    assert(real_env->localframe.front().size() == bridge_locals_before);
    assert(morrow_jni_runtime_release_app_bridge_start_params());
    assert(!morrow_jni_runtime_app_bridge_start_params_bound());
    assert(morrow_jni_runtime_app_bridge_start_params());
    assert(morrow_jni_runtime_release_app_bridge_init_params());
    assert(!morrow_jni_runtime_app_bridge_init_params_bound());
    assert(morrow_jni_runtime_app_bridge_init_params());
    main_env->DeleteLocalRef(start_class);
    main_env->DeleteLocalRef(platform_class);
    main_env->DeleteLocalRef(init_class);
    main_env->DeleteLocalRef(device_class);
    expected_activity = static_cast<jobject>(activity);
    expected_assets = static_cast<jobject>(asset_java);
    expected_configuration = static_cast<jobject>(configuration);
    expected_path = root;
    const size_t activity_locals_before = real_env->localframe.front().size();
    for (unsigned i = 0; i < 32; ++i) {
        assert(morrow_jni_runtime_call_game_activity(
            reinterpret_cast<uintptr_t>(&synthetic_game_activity_init),
            root, &init_result));
        assert(init_result.entry_invoked && init_result.entry_returned &&
               !init_result.java_exception_pending &&
               init_result.native_handle == 0x12345);
    }
    assert(native_activity_calls == 32);
    assert(real_env->localframe.front().size() == activity_locals_before);
    expected_activity = expected_assets = expected_configuration = nullptr;
    expected_path = nullptr;
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
    const size_t assets_locals_before = real_env->localframe.front().size();
    for (unsigned i = 0; i < 64; ++i) {
        jobject repeated = main_env->CallObjectMethod(
            static_cast<jobject>(activity), assets_method);
        assert(repeated && main_env->IsSameObject(repeated, activity_assets));
        main_env->DeleteLocalRef(repeated);
    }
    const size_t assets_locals_after = real_env->localframe.front().size();
    assert(assets_locals_after == assets_locals_before);
    jobject alias = main_env->NewLocalRef(static_cast<jobject>(asset_java));
    assert(alias && main_env->IsSameObject(alias, static_cast<jobject>(asset_java)));
    assert(from_java(main_env, alias) == native_manager);
    main_env->DeleteLocalRef(alias);
    assert(morrow_jni_runtime_bind_asset_manager(root) == asset_java);
    assert(!morrow_jni_runtime_bind_asset_manager("/tmp"));
    assert(morrow_jni_runtime_bind_horizon_surface(&horizon_window, 960, 540) == app_surface);
    assert(android_asset_compat_get_configuration(native_manager, &ndk_configuration));
    assert(ndk_configuration.screen_width_dp == 960 &&
           ndk_configuration.screen_height_dp == 540);
    from_ndk_assets(ndk_snapshot, native_manager);
    assert(ndk_width(ndk_snapshot) == 960 && ndk_height(ndk_snapshot) == 540);
    assert(morrow_jni_runtime_unbind_surface());
    assert(android_asset_compat_get_configuration(native_manager, &ndk_configuration));
    assert(ndk_configuration.screen_width_dp == 0 &&
           ndk_configuration.screen_height_dp == 0);
    from_ndk_assets(ndk_snapshot, native_manager);
    assert(ndk_width(ndk_snapshot) == 0 && ndk_height(ndk_snapshot) == 0);
    assert(ndk_configuration.language[0] == 'e' &&
           ndk_configuration.language[1] == 's');
    assert(!open_asset(native_manager, "../test.asset", 0));
    void *asset = open_asset(native_manager, "test.asset", 0);
    assert(asset && asset_length(asset) == 6);
    assert(morrow_jni_runtime_unbind_asset_manager());
    assert(!morrow_jni_runtime_asset_manager_bound());
    assert(!morrow_jni_runtime_asset_configuration_bound());
    from_ndk_assets(ndk_snapshot, native_manager);
    assert(ndk_width(ndk_snapshot) == 0 && ndk_height(ndk_snapshot) == 0);
    ndk_language(ndk_snapshot, ndk_locale);
    assert(ndk_locale[0] == 0 && ndk_locale[1] == 0);
    delete_ndk_config(ndk_snapshot);
    assert(!morrow_jni_runtime_app_bridge_start_params_bound());
    assert(!morrow_jni_runtime_app_bridge_start_params());
    assert(!morrow_jni_runtime_app_bridge_init_params_bound());
    assert(!morrow_jni_runtime_app_bridge_init_params());
    assert(morrow_jni_runtime_unbind_surface());
    assert(!from_java(main_env, asset_java));
    assert(!from_java(main_env, activity_assets));
    assert(!main_env->CallObjectMethod(static_cast<jobject>(activity), assets_method));
    main_env->DeleteLocalRef(activity_assets);
    assert(!open_asset(native_manager, "test.asset", 0));
    close_asset(asset);
    assert(morrow_jni_runtime_unbind_asset_manager());
    assert(morrow_jni_runtime_release_configuration());
    assert(!morrow_jni_runtime_configuration_bound());
    assert(main_env->IsInstanceOf(config_alias, config_class));
    jobject config_alias_2 = main_env->NewLocalRef(config_alias);
    assert(config_alias_2 && main_env->IsSameObject(config_alias, config_alias_2));
    // The Surface is now unbound, so measured viewport fields return to
    // unknown. Untested Android metadata remains unknown and writable.
    for (const char *field : {
             "colorMode", "densityDpi", "fontWeightAdjustment",
             "hardKeyboardHidden", "keyboard", "keyboardHidden", "mcc", "mnc",
             "navigation", "navigationHidden", "orientation", "screenHeightDp",
             "screenLayout", "screenWidthDp", "smallestScreenWidthDp", "uiMode"}) {
        jfieldID id = main_env->GetFieldID(config_class, field, "I");
        assert(id && main_env->GetIntField(config_alias, id) == 0);
        main_env->SetIntField(config_alias, id, 42);
        assert(main_env->GetIntField(config_alias_2, id) == 42);
        main_env->SetIntField(config_alias, id, 0);
    }
    jfieldID scale = main_env->GetFieldID(config_class, "fontScale", "F");
    assert(scale && main_env->GetFloatField(config_alias, scale) == 1.0f);
    main_env->SetFloatField(config_alias, scale, 1.25f);
    assert(main_env->GetFloatField(config_alias_2, scale) == 1.25f);
    main_env->SetFloatField(config_alias, scale, 1.0f);
    assert(main_env->GetIntField(config_alias, config_touch) == 3);
    main_env->SetIntField(config_alias, config_touch, 42);
    assert(main_env->GetIntField(config_alias_2, config_touch) == 42);
    main_env->SetIntField(config_alias, config_touch, 3);
    jclass locale_list_class = main_env->FindClass("android/os/LocaleList");
    jclass locale_class = main_env->FindClass("java/util/Locale");
    assert(locale_list_class && locale_class);
    jmethodID locales_method = main_env->GetMethodID(
        config_class, "getLocales", "()Landroid/os/LocaleList;");
    jmethodID locales_size = main_env->GetMethodID(locale_list_class, "size", "()I");
    jmethodID locales_get = main_env->GetMethodID(
        locale_list_class, "get", "(I)Ljava/util/Locale;");
    assert(locales_method && locales_size && locales_get);
    jmethodID locale_language = main_env->GetMethodID(
        locale_class, "getLanguage", "()Ljava/lang/String;");
    jmethodID locale_script = main_env->GetMethodID(
        locale_class, "getScript", "()Ljava/lang/String;");
    jmethodID locale_country = main_env->GetMethodID(
        locale_class, "getCountry", "()Ljava/lang/String;");
    jmethodID locale_variant = main_env->GetMethodID(
        locale_class, "getVariant", "()Ljava/lang/String;");
    assert(locale_language && locale_script && locale_country && locale_variant);
    jobject locales = main_env->CallObjectMethod(config_alias, locales_method);
    assert(locales && main_env->IsInstanceOf(locales, locale_list_class));
    assert(main_env->CallIntMethod(locales, locales_size) == 1);
    assert(!main_env->CallObjectMethod(locales, locales_get, -1));
    assert(!main_env->CallObjectMethod(locales, locales_get, 1));
    const size_t locale_locals_before = real_env->localframe.front().size();
    for (unsigned i = 0; i < 64; ++i) {
        jobject locale = main_env->CallObjectMethod(locales, locales_get, 0);
        assert(locale && main_env->IsInstanceOf(locale, locale_class));
        const struct { jmethodID method; const char *expected; } fields[] = {
            {locale_language, "es"}, {locale_script, ""},
            {locale_country, "419"}, {locale_variant, ""}
        };
        for (const auto &field : fields) {
            jstring result = static_cast<jstring>(main_env->CallObjectMethod(
                locale, field.method));
            assert(result);
            const char *contents = main_env->GetStringUTFChars(result, nullptr);
            assert(contents && !strcmp(contents, field.expected));
            main_env->ReleaseStringUTFChars(result, contents);
            main_env->DeleteLocalRef(result);
        }
        main_env->DeleteLocalRef(locale);
    }
    assert(!main_env->ExceptionCheck());
    assert(real_env->localframe.front().size() == locale_locals_before);
    main_env->DeleteLocalRef(locales);
    main_env->DeleteLocalRef(locale_class);
    main_env->DeleteLocalRef(locale_list_class);
    main_env->DeleteLocalRef(config_alias_2);
    void *next_configuration = morrow_jni_runtime_configuration();
    assert(next_configuration && !main_env->IsSameObject(
        config_alias, static_cast<jobject>(next_configuration)));
    assert(morrow_jni_runtime_release_configuration());
    main_env->DeleteLocalRef(config_alias);
    assert(morrow_jni_runtime_release_game_activity());
    assert(!morrow_jni_runtime_game_activity_ready());
    assert(!morrow_jni_runtime_activity_requests(&activity_requests));
    assert(morrow_jni_runtime_release_game_activity());
    assert(unlink(asset_path) == 0 && rmdir(root) == 0);
    puts("morrow_jni_runtime_test: JNI_OnLoad, storage, live Java/NDK Configuration, DisplayMetrics, LocaleList, ClassLoader and references passed");
    return 0;
}
