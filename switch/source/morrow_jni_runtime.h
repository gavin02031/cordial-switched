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

#ifdef __cplusplus
}
#endif

#endif
