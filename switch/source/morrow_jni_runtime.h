#ifndef MORROW_JNI_RUNTIME_H
#define MORROW_JNI_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opt-in Horizon libjnivm runtime. This provides only a partial GameActivity,
// not the complete Android services or an executable Roblox engine image.
bool morrow_jni_runtime_start(void);

// Return the actual libjnivm JavaVM, not the separate diagnostic invocation
// bridge. JNIEnv::GetJavaVM must return the same JavaVM passed to JNI_OnLoad.
void *morrow_jni_runtime_java_vm(void);

// The JNI environment belongs to the calling, already attached thread.
// Returns NULL before startup or after detaching, never implicitly attaches.
void *morrow_jni_runtime_jni_env(void);

typedef struct {
    bool vm_available;
    bool jni_onload_accepted;
    bool native_registered;
    bool native_invoked;
    bool native_unregistered;
    bool local_references_released;
} MorrowJniOnLoadProbe;

// Call an original, Morrow-owned JNI_OnLoad-shaped function against the real
// VM and invoke one registered (I)I native through CallStaticIntMethod. The
// bounded patched VM rejects other signatures until their ABI can be handled
// correctly. No user library or Roblox code runs; repeats must not retain
// JNI locals or native registrations.
bool morrow_jni_runtime_probe_onload(MorrowJniOnLoadProbe *report);

// A host-owned, live Horizon NWindow can be represented by one stable JNI
// android/view/Surface reference. The caller keeps NWindow alive until unbind.
// The returned jobject is a real libjnivm global reference, NOT an NWindow.
// A new native window cannot silently replace the current surface's owner.
void *morrow_jni_runtime_bind_horizon_surface(void *horizon_window,
                                               int32_t width, int32_t height);
bool morrow_jni_runtime_surface_bound(void);
bool morrow_jni_runtime_unbind_surface(void);

// Bind a real JNI android/content/res/AssetManager global reference to a
// caller-owned extracted asset directory. Missing directories fail closed;
// changing the owning directory requires unbinding the previous reference.
// Files are accessed read-only through the existing bounded NDK adapter.
void *morrow_jni_runtime_bind_asset_manager(const char *asset_root);
bool morrow_jni_runtime_asset_manager_bound(void);
bool morrow_jni_runtime_unbind_asset_manager(void);

// Partial, typed App Bridge V2 StartAppParams for the observed Android shell
// path. Created only with a live Horizon Surface AND a real asset directory.
// This proves JNI field/getter and Surface identity, not the other startup
// prerequisites: InitParams, DeviceParams, callbacks and login remain absent.
// Do not pass this object to Roblox until those requirements are implemented.
void *morrow_jni_runtime_app_bridge_start_params(void);
bool morrow_jni_runtime_app_bridge_start_params_bound(void);
bool morrow_jni_runtime_release_app_bridge_start_params(void);

// Typed, *partial* App Bridge InitParams with shared real PlatformParams and
// observable device facts. Android app version, User-Agent, API level,
// country, memory and VR Activity are unknown: this object must not yet be
// passed to libroblox.so as though Android startup had been completed.
void *morrow_jni_runtime_app_bridge_init_params(void);
bool morrow_jni_runtime_app_bridge_init_params_bound(void);
bool morrow_jni_runtime_release_app_bridge_init_params(void);

// Typed, stable Java Configuration identity for the eventual GameActivity
// native initialiser. This is deliberately not an invented Android Resources
// object or populated Configuration: screen/locale data remain unavailable.
void *morrow_jni_runtime_configuration(void);
bool morrow_jni_runtime_configuration_bound(void);
bool morrow_jni_runtime_release_configuration(void);

// GameActivity has typed insets and asset callbacks, a restricted ClassLoader
// for explicitly registered platform classes, and records two Android
// window/input requests. No APK classpath, Android window manager or IME
// applies those requests; lifecycle and app bootstrap remain unimplemented.
void *morrow_jni_runtime_game_activity(void);
bool morrow_jni_runtime_game_activity_ready(void);
bool morrow_jni_runtime_release_game_activity(void);

typedef struct {
    uint32_t window_flag_requests;
    int32_t last_window_flags;
    int32_t last_window_mask;
    uint32_t ime_editor_info_requests;
    int32_t last_ime_value_1;
    int32_t last_ime_value_2;
    int32_t last_ime_value_3;
} MorrowGameActivityRequests;

// Returns false if no Activity is registered. Observations are diagnostic,
// not confirmation that Android window flags or soft keyboard took effect.
bool morrow_jni_runtime_activity_requests(MorrowGameActivityRequests *requests);

typedef struct {
    bool entry_invoked;
    bool entry_returned;
    bool java_exception_pending;
    int64_t native_handle;
} MorrowGameActivityInitResult;

// This is the observed AGDK JNI descriptor from Cordial's Android trace:
// (JNIEnv*, jobject, jstring, jstring, jstring, AssetManager, savedState,
// Configuration) -> jlong. It is NOT generic ABI discovery. Call only after
// a successful real JNI_OnLoad and an explicit user-initiated experiment.
// A missing asset manager is a hard failure; no placeholder is fabricated.
bool morrow_jni_runtime_call_game_activity(uintptr_t entry, const char *data_path,
                                            MorrowGameActivityInitResult *result);

#ifdef __cplusplus
}
#endif

#endif
