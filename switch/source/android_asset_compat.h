#ifndef CORDIAL_SWITCH_ANDROID_ASSET_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_ASSET_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration is supplied by a verified JNI/Android-runtime owner. A bare
// asset directory implies no locale or dp-density: Morrow's JNI owner can
// explicitly supply the Switch system language and a neutral 1x viewport
// layout derived from a real Horizon Surface. Language/country are exactly
// two bytes, not terminated.
typedef struct {
    char language[2];
    char country[2];
    int32_t screen_size;
    int32_t screen_width_dp;
    int32_t screen_height_dp;
    int32_t nav_hidden;
} AndroidAssetConfiguration;

// The JNI layer owns the Java AssetManager and its global reference. Only
// verified Java objects may be bound to a read-only extracted asset directory.
// Without this binding, AAssetManager_fromJava returns NULL.
bool android_asset_compat_bind_manager(void *java_manager, const char *asset_root);
void android_asset_compat_unbind_manager(void *java_manager);

// A jobject returned by a JNI method may have a different reference address
// from the bound global jobject while naming the same Java object. The VM
// owner alone can validate that identity with IsSameObject. The matcher is
// optional: without an installed VM, only exact bound references work.
typedef bool (*AndroidAssetJavaSameObject)(void *env, void *candidate,
                                           void *bound_reference);
void android_asset_compat_set_reference_matcher(AndroidAssetJavaSameObject matcher);

// The native AAssetManager handle returned by AAssetManager_fromJava is the
// identity for these calls, not the original jobject. Both fail on stale or
// unbound handles. Unbinding removes any previously supplied configuration.
bool android_asset_compat_set_configuration(void *native_manager,
                                             const AndroidAssetConfiguration *configuration);
bool android_asset_compat_get_configuration(const void *native_manager,
                                             AndroidAssetConfiguration *configuration);

bool android_asset_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address);

#ifdef __cplusplus
}
#endif

#endif
