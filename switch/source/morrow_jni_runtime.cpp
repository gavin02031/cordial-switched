// The pinned libjnivm VM is used here, rather than imitating Java objects.
#include <string> // Pinned vm.h requires std::hash<string> before unordered_map.
#include <jnivm.h>

#include "morrow_jni_runtime.h"
#include "android_jni_vm_bridge.h"
#include "android_asset_compat.h"
#include "android_native_window_compat.h"

#include <memory>
#include <mutex>
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace {

std::mutex start_mutex;
std::unique_ptr<jnivm::VM> runtime;
thread_local bool attached_to_runtime;

class HorizonSurface : public jnivm::Object {};
std::shared_ptr<HorizonSurface> surface_owner;
jobject surface_reference;
void *surface_window;
int32_t surface_width, surface_height;

class AssetManagerObject : public jnivm::Object {};
std::shared_ptr<AssetManagerObject> asset_owner;
jobject asset_reference;
std::string asset_directory;

// Horizon's settings service supplies a BCP-47 language code. Keep that
// value separate from Android's unrelated locale defaults: an unavailable
// service must leave the Java LocaleList empty, not invent en-US.
struct SystemLocale {
    std::string tag, language, script, country;
};
SystemLocale system_locale;

bool parse_system_locale(const char *tag, SystemLocale *parsed)
{
    if (!tag || !parsed) return false;
    const std::string value(tag);
    if (value.size() < 2 || value.size() > 16) return false;
    SystemLocale candidate;
    candidate.tag = value;
    size_t start = 0;
    unsigned part = 0;
    while (start < value.size()) {
        const size_t end = value.find('-', start);
        const std::string segment = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const bool letters = !segment.empty() &&
            std::all_of(segment.begin(), segment.end(), [](unsigned char c) {
                return std::isalpha(c) && c < 128;
            });
        const bool digits = !segment.empty() &&
            std::all_of(segment.begin(), segment.end(), [](unsigned char c) {
                return std::isdigit(c) && c < 128;
            });
        if (part == 0 && letters && (segment.size() == 2 || segment.size() == 3))
            candidate.language = segment;
        else if (part > 0 && candidate.script.empty() && candidate.country.empty() &&
                 letters && segment.size() == 4)
            candidate.script = segment;
        else if (part > 0 && candidate.country.empty() &&
                 ((letters && segment.size() == 2) ||
                  (digits && segment.size() == 3)))
            candidate.country = segment;
        else
            return false;
        ++part;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (part == 0 || value.back() == '-') return false;
    *parsed = std::move(candidate);
    return true;
}

// GameActivity reads the Java Configuration fields and LocaleList before it
// reaches the game's native callbacks. Unknown values remain explicitly zero;
// neither a device locale nor dp/density is inferred from 720p pixels.
class HorizonLocale : public jnivm::Object {
public:
    std::shared_ptr<jnivm::String> language, script, country, variant;
    explicit HorizonLocale(const SystemLocale &locale)
        : language(std::make_shared<jnivm::String>(locale.language)),
          script(std::make_shared<jnivm::String>(locale.script)),
          country(std::make_shared<jnivm::String>(locale.country)),
          variant(std::make_shared<jnivm::String>("")) {}

    std::shared_ptr<jnivm::String> getLanguage(jnivm::ENV *) { return language; }
    std::shared_ptr<jnivm::String> getScript(jnivm::ENV *) { return script; }
    std::shared_ptr<jnivm::String> getCountry(jnivm::ENV *) { return country; }
    std::shared_ptr<jnivm::String> getVariant(jnivm::ENV *) { return variant; }

    static void Register(jnivm::ENV *env)
    {
        auto klass = env->GetClass<HorizonLocale>("java/util/Locale");
        klass->HookInstanceFunction(env, "getLanguage", &HorizonLocale::getLanguage);
        klass->HookInstanceFunction(env, "getScript", &HorizonLocale::getScript);
        klass->HookInstanceFunction(env, "getCountry", &HorizonLocale::getCountry);
        klass->HookInstanceFunction(env, "getVariant", &HorizonLocale::getVariant);
    }
};

class HorizonLocaleList : public jnivm::Object {
public:
    std::shared_ptr<HorizonLocale> primary;
    explicit HorizonLocaleList(const SystemLocale &locale)
    {
        if (!locale.tag.empty()) primary = std::make_shared<HorizonLocale>(locale);
    }
    jint size(jnivm::ENV *) { return primary ? 1 : 0; }
    std::shared_ptr<HorizonLocale> get(jnivm::ENV *, jint index)
    {
        return index == 0 ? primary : nullptr;
    }

    static void Register(jnivm::ENV *env)
    {
        auto klass = env->GetClass<HorizonLocaleList>("android/os/LocaleList");
        klass->HookInstanceFunction(env, "size", &HorizonLocaleList::size);
        klass->HookInstanceFunction(env, "get", &HorizonLocaleList::get);
    }
};

class HorizonConfiguration : public jnivm::Object {
public:
    jint colorMode = 0, densityDpi = 0, fontWeightAdjustment = 0;
    jint hardKeyboardHidden = 0, keyboard = 0, keyboardHidden = 0;
    jint mcc = 0, mnc = 0, navigation = 0, navigationHidden = 0;
    jint orientation = 0, screenHeightDp = 0, screenLayout = 0;
    jint screenWidthDp = 0, smallestScreenWidthDp = 0, touchscreen = 0;
    jint uiMode = 0;
    jfloat fontScale = 0;
    std::shared_ptr<HorizonLocaleList> locale_list;

    explicit HorizonConfiguration(const SystemLocale &locale)
        : locale_list(std::make_shared<HorizonLocaleList>(locale)) {}

    std::shared_ptr<HorizonLocaleList> getLocales(jnivm::ENV *)
    {
        return locale_list;
    }

    static void Register(jnivm::ENV *env)
    {
        HorizonLocale::Register(env);
        HorizonLocaleList::Register(env);
        auto klass = env->GetClass<HorizonConfiguration>("android/content/res/Configuration");
#define HOOK_CONFIG_FIELD(name) klass->HookInstance(env, #name, &HorizonConfiguration::name)
        HOOK_CONFIG_FIELD(colorMode);
        HOOK_CONFIG_FIELD(densityDpi);
        HOOK_CONFIG_FIELD(fontScale);
        HOOK_CONFIG_FIELD(fontWeightAdjustment);
        HOOK_CONFIG_FIELD(hardKeyboardHidden);
        HOOK_CONFIG_FIELD(keyboard);
        HOOK_CONFIG_FIELD(keyboardHidden);
        HOOK_CONFIG_FIELD(mcc);
        HOOK_CONFIG_FIELD(mnc);
        HOOK_CONFIG_FIELD(navigation);
        HOOK_CONFIG_FIELD(navigationHidden);
        HOOK_CONFIG_FIELD(orientation);
        HOOK_CONFIG_FIELD(screenHeightDp);
        HOOK_CONFIG_FIELD(screenLayout);
        HOOK_CONFIG_FIELD(screenWidthDp);
        HOOK_CONFIG_FIELD(smallestScreenWidthDp);
        HOOK_CONFIG_FIELD(touchscreen);
        HOOK_CONFIG_FIELD(uiMode);
#undef HOOK_CONFIG_FIELD
        klass->HookInstanceFunction(env, "getLocales", &HorizonConfiguration::getLocales);
    }
};
std::shared_ptr<HorizonConfiguration> configuration_owner;
jobject configuration_reference;

// Cordial's observed Android launch trace reaches StartAppParams.surface
// through App Bridge V2, not through AGDK's GameActivity window lifecycle.
// Define the verified Java field types here so future calls can use the real
// libjnivm JNI table. Unknown Android state is not fabricated: physical size
// and signed-in identity are absent, and no engine startup uses this yet.
class HorizonPlatformParams : public jnivm::Object {
public:
    std::shared_ptr<jnivm::String> assetFolderPath;
    jfloat dpiScale = 1.0f; // Neutral scale, not a measured Android density.
    jboolean isKeyboardDevice = JNI_FALSE;
    jboolean isMouseDevice = JNI_FALSE;
    jboolean isTouchDevice = JNI_TRUE; // Switch Lite's actual touchscreen.
    jint viewportWidthMm = 0, viewportHeightMm = 0; // No physical size reading.

    std::shared_ptr<jnivm::String> get_assetFolderPath(jnivm::ENV *) { return assetFolderPath; }
    jfloat get_dpiScale(jnivm::ENV *) { return dpiScale; }
    jboolean get_isKeyboardDevice(jnivm::ENV *) { return isKeyboardDevice; }
    jboolean get_isMouseDevice(jnivm::ENV *) { return isMouseDevice; }
    jboolean get_isTouchDevice(jnivm::ENV *) { return isTouchDevice; }
    jint get_viewportWidthMm(jnivm::ENV *) { return viewportWidthMm; }
    jint get_viewportHeightMm(jnivm::ENV *) { return viewportHeightMm; }

    static void Register(jnivm::ENV *env)
    {
        auto klass = env->GetClass<HorizonPlatformParams>(
            "com/roblox/engine/jni/model/PlatformParams");
#define PLATFORM_FIELD(name) klass->HookInstance(env, #name, &HorizonPlatformParams::name)
        PLATFORM_FIELD(assetFolderPath);
        PLATFORM_FIELD(dpiScale);
        PLATFORM_FIELD(isKeyboardDevice);
        PLATFORM_FIELD(isMouseDevice);
        PLATFORM_FIELD(isTouchDevice);
        PLATFORM_FIELD(viewportWidthMm);
        PLATFORM_FIELD(viewportHeightMm);
#undef PLATFORM_FIELD
#define PLATFORM_GETTER(name) klass->HookInstanceFunction(env, #name, &HorizonPlatformParams::get_##name)
        PLATFORM_GETTER(assetFolderPath);
        PLATFORM_GETTER(dpiScale);
        PLATFORM_GETTER(isKeyboardDevice);
        PLATFORM_GETTER(isMouseDevice);
        PLATFORM_GETTER(isTouchDevice);
        PLATFORM_GETTER(viewportWidthMm);
        PLATFORM_GETTER(viewportHeightMm);
#undef PLATFORM_GETTER
    }
};

std::shared_ptr<HorizonPlatformParams> platform_owner;

std::shared_ptr<HorizonPlatformParams> platform_for_assets_locked()
{
    if (asset_directory.empty()) return nullptr;
    if (!platform_owner) {
        auto platform = std::make_shared<HorizonPlatformParams>();
        platform->assetFolderPath = std::make_shared<jnivm::String>(asset_directory);
        platform_owner = std::move(platform);
    }
    return platform_owner;
}

// DeviceParams is a field-carrying Java class, unlike AutoValue InitParams.
// Only values known about the actual host are filled. Reporting Android API
// 33 or an invented Roblox version would mislead the engine's graphics gate.
class HorizonDeviceParams : public jnivm::Object {
public:
    std::shared_ptr<jnivm::String> appBuildVariant, appVersion, country, deviceName;
    std::shared_ptr<jnivm::String> deviceSku, displayResolution, manufacturer;
    std::shared_ptr<jnivm::String> networkType, osVersion, socModel, testDeviceName;
    jboolean cpu64Bit = JNI_TRUE, isChrome = JNI_FALSE, isLowRamDevice = JNI_FALSE;
    jint deviceTotalMemoryMB = 0, displayPhysicalHeightPixels = 0;
    jint displayPhysicalWidthPixels = 0, largeMemoryClass = 0, memoryClass = 0;
    jlong lowMemoryKillerBackgroundAppThreshold = 0;
    jlong lowMemoryKillerForegroundAppThreshold = 0;

    static void Register(jnivm::ENV *env)
    {
        auto klass = env->GetClass<HorizonDeviceParams>(
            "com/roblox/engine/jni/model/DeviceParams");
#define DEVICE_FIELD(name) klass->HookInstance(env, #name, &HorizonDeviceParams::name)
        DEVICE_FIELD(appBuildVariant);
        DEVICE_FIELD(appVersion);
        DEVICE_FIELD(country);
        DEVICE_FIELD(deviceName);
        DEVICE_FIELD(deviceSku);
        DEVICE_FIELD(displayResolution);
        DEVICE_FIELD(manufacturer);
        DEVICE_FIELD(networkType);
        DEVICE_FIELD(osVersion);
        DEVICE_FIELD(socModel);
        DEVICE_FIELD(testDeviceName);
        DEVICE_FIELD(cpu64Bit);
        DEVICE_FIELD(isChrome);
        DEVICE_FIELD(isLowRamDevice);
        DEVICE_FIELD(deviceTotalMemoryMB);
        DEVICE_FIELD(displayPhysicalHeightPixels);
        DEVICE_FIELD(displayPhysicalWidthPixels);
        DEVICE_FIELD(largeMemoryClass);
        DEVICE_FIELD(memoryClass);
        DEVICE_FIELD(lowMemoryKillerBackgroundAppThreshold);
        DEVICE_FIELD(lowMemoryKillerForegroundAppThreshold);
#undef DEVICE_FIELD
    }
};

class HorizonInitParams : public jnivm::Object {
public:
    std::shared_ptr<jnivm::String> baseURL, buildVariant, userAgent;
    std::shared_ptr<HorizonDeviceParams> deviceParams;
    std::shared_ptr<HorizonPlatformParams> platformParams;
    jboolean isPotato = JNI_FALSE, isTablet = JNI_FALSE, isVrDevice = JNI_FALSE;

    std::shared_ptr<jnivm::String> get_baseURL(jnivm::ENV *) { return baseURL; }
    std::shared_ptr<jnivm::String> get_buildVariant(jnivm::ENV *) { return buildVariant; }
    std::shared_ptr<jnivm::String> get_userAgent(jnivm::ENV *) { return userAgent; }
    std::shared_ptr<HorizonDeviceParams> get_deviceParams(jnivm::ENV *) { return deviceParams; }
    std::shared_ptr<HorizonPlatformParams> get_platformParams(jnivm::ENV *) { return platformParams; }
    jboolean get_isPotato(jnivm::ENV *) { return isPotato; }
    jboolean get_isTablet(jnivm::ENV *) { return isTablet; }
    jboolean get_isVrDevice(jnivm::ENV *) { return isVrDevice; }

    static void Register(jnivm::ENV *env)
    {
        HorizonDeviceParams::Register(env);
        auto klass = env->GetClass<HorizonInitParams>(
            "com/roblox/engine/jni/autovalue/InitParams");
#define INIT_FIELD(name) klass->HookInstance(env, #name, &HorizonInitParams::name)
        INIT_FIELD(baseURL);
        INIT_FIELD(buildVariant);
        INIT_FIELD(userAgent);
        INIT_FIELD(deviceParams);
        INIT_FIELD(platformParams);
        INIT_FIELD(isPotato);
        INIT_FIELD(isTablet);
        INIT_FIELD(isVrDevice);
#undef INIT_FIELD
#define INIT_GETTER(name) klass->HookInstanceFunction(env, #name, &HorizonInitParams::get_##name)
        INIT_GETTER(baseURL);
        INIT_GETTER(buildVariant);
        INIT_GETTER(userAgent);
        INIT_GETTER(deviceParams);
        INIT_GETTER(platformParams);
        INIT_GETTER(isPotato);
        INIT_GETTER(isTablet);
        INIT_GETTER(isVrDevice);
#undef INIT_GETTER
        // Android vrContext requires a real Activity. There is none yet.
    }
};

std::shared_ptr<HorizonInitParams> init_params_owner;
jobject init_params_reference;

void release_init_params_locked()
{
    if (!init_params_reference) return;
    runtime->GetJNIEnv()->DeleteGlobalRef(init_params_reference);
    init_params_reference = nullptr;
    init_params_owner.reset();
}

class HorizonStartAppParams : public jnivm::Object {
public:
    std::shared_ptr<jnivm::String> appStarterPlace, appStarterScript;
    std::shared_ptr<jnivm::String> selectedTheme, username;
    std::shared_ptr<HorizonPlatformParams> platformParams;
    std::shared_ptr<HorizonSurface> surface;
    jlong appUserId = 0; // No account loaded on Horizon.
    jboolean isUnder13 = JNI_FALSE; // No signed-in account age to report.
    jint membershipType = 0;

    std::shared_ptr<jnivm::String> get_appStarterPlace(jnivm::ENV *) { return appStarterPlace; }
    std::shared_ptr<jnivm::String> get_appStarterScript(jnivm::ENV *) { return appStarterScript; }
    std::shared_ptr<jnivm::String> get_selectedTheme(jnivm::ENV *) { return selectedTheme; }
    std::shared_ptr<jnivm::String> get_username(jnivm::ENV *) { return username; }
    std::shared_ptr<HorizonPlatformParams> get_platformParams(jnivm::ENV *) { return platformParams; }
    std::shared_ptr<HorizonSurface> get_surface(jnivm::ENV *) { return surface; }
    jlong get_appUserId(jnivm::ENV *) { return appUserId; }
    jboolean get_isUnder13(jnivm::ENV *) { return isUnder13; }
    jint get_membershipType(jnivm::ENV *) { return membershipType; }

    static void Register(jnivm::ENV *env)
    {
        HorizonPlatformParams::Register(env);
        auto klass = env->GetClass<HorizonStartAppParams>(
            "com/roblox/engine/jni/autovalue/StartAppParams");
#define APP_FIELD(name) klass->HookInstance(env, #name, &HorizonStartAppParams::name)
        APP_FIELD(appStarterPlace);
        APP_FIELD(appStarterScript);
        APP_FIELD(selectedTheme);
        APP_FIELD(username);
        APP_FIELD(platformParams);
        APP_FIELD(surface);
        APP_FIELD(appUserId);
        APP_FIELD(isUnder13);
        APP_FIELD(membershipType);
#undef APP_FIELD
#define APP_GETTER(name) klass->HookInstanceFunction(env, #name, &HorizonStartAppParams::get_##name)
        APP_GETTER(appStarterPlace);
        APP_GETTER(appStarterScript);
        APP_GETTER(selectedTheme);
        APP_GETTER(username);
        APP_GETTER(platformParams);
        APP_GETTER(surface);
        APP_GETTER(appUserId);
        APP_GETTER(isUnder13);
        APP_GETTER(membershipType);
#undef APP_GETTER
        // vrContext has no working Android Activity/VR service on Horizon.
        // Leaving that member unresolved is intentional until it exists.
    }
};

std::shared_ptr<HorizonStartAppParams> start_app_owner;
jobject start_app_reference;

void release_start_app_params_locked()
{
    if (!start_app_reference) return;
    runtime->GetJNIEnv()->DeleteGlobalRef(start_app_reference);
    start_app_reference = nullptr;
    start_app_owner.reset();
}

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
            "android/content/res/Configuration",
            "android/os/LocaleList", "java/util/Locale",
            "com/roblox/engine/jni/model/PlatformParams",
            "com/roblox/engine/jni/model/DeviceParams",
            "com/roblox/engine/jni/autovalue/InitParams",
            "com/roblox/engine/jni/autovalue/StartAppParams",
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
        HorizonConfiguration::Register(env);
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
        HorizonStartAppParams::Register(candidate->GetEnv().get());
        HorizonInitParams::Register(candidate->GetEnv().get());
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

extern "C" bool morrow_jni_runtime_bind_system_locale(const char *language_tag)
{
    SystemLocale proposed;
    if (!parse_system_locale(language_tag, &proposed)) return false;
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime) return false;
    // A previously handed-out Configuration must not silently change while
    // engine Java code holds aliases to its LocaleList.
    if (!system_locale.tag.empty())
        return proposed.tag == system_locale.tag;
    if (configuration_reference) return false;
    system_locale = std::move(proposed);
    return true;
}

extern "C" bool morrow_jni_runtime_system_locale_bound(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return !system_locale.tag.empty();
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
        if (surface_width != width || surface_height != height)
            release_init_params_locked();
        surface_width = width;
        surface_height = height;
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
        surface_width = width;
        surface_height = height;
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
    release_init_params_locked();
    release_start_app_params_locked();
    android_native_window_compat_unbind_surface(surface_reference);
    runtime->GetJNIEnv()->DeleteGlobalRef(surface_reference);
    surface_reference = nullptr;
    surface_window = nullptr;
    surface_width = surface_height = 0;
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

extern "C" void *morrow_jni_runtime_app_bridge_start_params(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    // A Java Surface alone is not a drawable window: it must still have its
    // registered NWindow owner. The asset folder has the same ownership rule.
    if (!runtime || !attached_to_runtime || !surface_reference || !surface_owner ||
        !asset_reference || asset_directory.empty())
        return nullptr;
    if (start_app_reference)
        return start_app_reference;
    try {
        jnivm::ENV *env = runtime->GetEnv().get();
        auto platform = platform_for_assets_locked();
        if (!platform) return nullptr;
        auto owner = std::make_shared<HorizonStartAppParams>();
        // The Android app's default shell uses empty starter strings. No
        // account has been authenticated on Horizon, so username remains
        // empty rather than borrowing an unrelated player's credentials.
        owner->appStarterPlace = std::make_shared<jnivm::String>("");
        owner->appStarterScript = std::make_shared<jnivm::String>("");
        owner->username = std::make_shared<jnivm::String>("");
        owner->platformParams = std::move(platform);
        owner->surface = surface_owner;
        JNIEnv *jni = env->GetJNIEnv();
        jobject local = jnivm::JNITypes<std::shared_ptr<HorizonStartAppParams>>::ToJNIType(
            env, owner);
        jobject global = jni->NewGlobalRef(local);
        jni->DeleteLocalRef(local);
        if (!global) return nullptr;
        start_app_owner = std::move(owner);
        start_app_reference = global;
        return global;
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool morrow_jni_runtime_app_bridge_start_params_bound(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return start_app_reference != nullptr && surface_reference != nullptr &&
           asset_reference != nullptr;
}

extern "C" bool morrow_jni_runtime_release_app_bridge_start_params(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!start_app_reference) return true;
    if (!runtime || !attached_to_runtime) return false;
    release_start_app_params_locked();
    return true;
}

extern "C" void *morrow_jni_runtime_app_bridge_init_params(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime || !asset_reference || asset_directory.empty())
        return nullptr;
    if (init_params_reference)
        return init_params_reference;
    try {
        jnivm::ENV *env = runtime->GetEnv().get();
        auto platform = platform_for_assets_locked();
        if (!platform) return nullptr;
        auto device = std::make_shared<HorizonDeviceParams>();
        device->appBuildVariant = std::make_shared<jnivm::String>("release");
        device->deviceName = std::make_shared<jnivm::String>("Morrow");
        device->deviceSku = std::make_shared<jnivm::String>("switch_lite");
        device->manufacturer = std::make_shared<jnivm::String>("Nintendo");
        if (surface_width > 0 && surface_height > 0) {
            char resolution[48];
            snprintf(resolution, sizeof(resolution), "%dx%d",
                     surface_width, surface_height);
            device->displayResolution = std::make_shared<jnivm::String>(resolution);
            device->displayPhysicalWidthPixels = surface_width;
            device->displayPhysicalHeightPixels = surface_height;
        }
        auto owner = std::make_shared<HorizonInitParams>();
        owner->baseURL = std::make_shared<jnivm::String>("https://www.roblox.com/");
        owner->buildVariant = std::make_shared<jnivm::String>("release");
        owner->deviceParams = std::move(device);
        owner->platformParams = std::move(platform);
        JNIEnv *jni = env->GetJNIEnv();
        jobject local = jnivm::JNITypes<std::shared_ptr<HorizonInitParams>>::ToJNIType(
            env, owner);
        jobject global = jni->NewGlobalRef(local);
        jni->DeleteLocalRef(local);
        if (!global) return nullptr;
        init_params_owner = std::move(owner);
        init_params_reference = global;
        return global;
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool morrow_jni_runtime_app_bridge_init_params_bound(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return init_params_reference != nullptr && asset_reference != nullptr;
}

extern "C" bool morrow_jni_runtime_release_app_bridge_init_params(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!init_params_reference) return true;
    if (!runtime || !attached_to_runtime) return false;
    release_init_params_locked();
    return true;
}

extern "C" void *morrow_jni_runtime_configuration(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!runtime || !attached_to_runtime)
        return nullptr;
    if (configuration_reference)
        return configuration_reference;
    try {
        jnivm::ENV *env = runtime->GetEnv().get();
        auto owner = std::make_shared<HorizonConfiguration>(system_locale);
        JNIEnv *jni = env->GetJNIEnv();
        jobject local = jnivm::JNITypes<std::shared_ptr<HorizonConfiguration>>::ToJNIType(
            env, owner);
        jobject global = jni->NewGlobalRef(local);
        jni->DeleteLocalRef(local);
        if (!global)
            return nullptr;
        configuration_owner = std::move(owner);
        configuration_reference = global;
        return global;
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool morrow_jni_runtime_configuration_bound(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    return configuration_reference != nullptr;
}

extern "C" bool morrow_jni_runtime_release_configuration(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!configuration_reference)
        return true;
    if (!runtime || !attached_to_runtime)
        return false;
    runtime->GetJNIEnv()->DeleteGlobalRef(configuration_reference);
    configuration_reference = nullptr;
    configuration_owner.reset();
    return true;
}

extern "C" bool morrow_jni_runtime_unbind_asset_manager(void)
{
    std::lock_guard<std::mutex> lock(start_mutex);
    if (!asset_reference)
        return true;
    if (!runtime || !attached_to_runtime)
        return false;
    release_init_params_locked();
    release_start_app_params_locked();
    android_asset_compat_unbind_manager(asset_reference);
    runtime->GetJNIEnv()->DeleteGlobalRef(asset_reference);
    asset_reference = nullptr;
    asset_directory.clear();
    platform_owner.reset();
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

extern "C" bool morrow_jni_runtime_call_game_activity(
    uintptr_t entry, const char *data_path, MorrowGameActivityInitResult *result)
{
    if (!result) return false;
    *result = {};
    if (!entry || !data_path || !*data_path) return false;

    // Do not hold start_mutex while calling guest code: GameActivity's
    // getAssets and window callbacks acquire it during native initialization.
    JNIEnv *env = nullptr;
    jobject activity = nullptr, assets = nullptr, configuration = nullptr;
    {
        std::lock_guard<std::mutex> lock(start_mutex);
        if (!runtime || !attached_to_runtime || !game_activity_reference ||
            !asset_reference || !configuration_reference) return false;
        env = runtime->GetJNIEnv();
        activity = env->NewLocalRef(game_activity_reference);
        assets = env->NewLocalRef(asset_reference);
        configuration = env->NewLocalRef(configuration_reference);
    }
    if (!activity || !assets || !configuration) {
        if (activity) env->DeleteLocalRef(activity);
        if (assets) env->DeleteLocalRef(assets);
        if (configuration) env->DeleteLocalRef(configuration);
        return false;
    }

    jstring internal = nullptr, obb = nullptr, external = nullptr;
    bool returned = false;
    try {
        internal = env->NewStringUTF(data_path);
        obb = env->NewStringUTF(data_path);
        external = env->NewStringUTF(data_path);
        if (internal && obb && external) {
            using NativeInit = jlong (*)(JNIEnv *, jobject, jstring, jstring,
                                        jstring, jobject, jbyteArray, jobject);
            // A cold start has no saved Android state. The three paths point
            // to the real app-owned SD folder; no APK or account data is read.
            result->entry_invoked = true;
            result->native_handle = reinterpret_cast<NativeInit>(entry)(
                env, activity, internal, obb, external, assets, nullptr,
                configuration);
            result->entry_returned = true;
            result->java_exception_pending = env->ExceptionCheck();
            returned = true;
        }
    } catch (...) {
        // An Android callback may throw C++ in this incomplete VM. Do not
        // claim that the engine started after a failed boundary crossing.
    }
    if (external) env->DeleteLocalRef(external);
    if (obb) env->DeleteLocalRef(obb);
    if (internal) env->DeleteLocalRef(internal);
    env->DeleteLocalRef(configuration);
    env->DeleteLocalRef(assets);
    env->DeleteLocalRef(activity);
    return returned;
}
