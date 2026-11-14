#ifndef MORROW_JNI_RUNTIME_H
#define MORROW_JNI_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opt-in Horizon libjnivm runtime. This does not implement GameActivity,
// a full Android resources service or any executable Roblox engine image.
bool morrow_jni_runtime_start(void);

// Return the actual libjnivm JavaVM, not the separate diagnostic invocation
// bridge. JNIEnv::GetJavaVM must return the same JavaVM passed to JNI_OnLoad.
void *morrow_jni_runtime_java_vm(void);

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

// The initial GameActivity reference has the correct JNI class identity and
// one truthful callback: zero waterfall insets on the full-screen Switch.
// Native lifecycle, IME, flag bootstrap and app services are not implemented.
void *morrow_jni_runtime_game_activity(void);
bool morrow_jni_runtime_game_activity_ready(void);
bool morrow_jni_runtime_release_game_activity(void);

#ifdef __cplusplus
}
#endif

#endif
