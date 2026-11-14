#ifndef CORDIAL_SWITCH_ANDROID_ASSET_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_ASSET_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// The JNI layer owns the Java AssetManager and its global reference. Only
// verified Java objects may be bound to a read-only extracted asset directory.
// Without this binding, AAssetManager_fromJava returns NULL.
bool android_asset_compat_bind_manager(void *java_manager, const char *asset_root);
void android_asset_compat_unbind_manager(void *java_manager);

bool android_asset_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address);

#endif
