#include "android_configuration_compat.h"
#include "android_asset_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(*NewConfiguration)(void);
typedef void (*DeleteConfiguration)(void *);
typedef void (*FromAssetManager)(void *, void *);
typedef void (*GetLocale)(void *, char *);
typedef int32_t (*GetInteger)(void *);
typedef void *(*FromJava)(void *, void *);

static void resolved(const char *name, void *destination)
{
    uint64_t address = 0;
    assert(android_configuration_compat_resolve(name, NULL, NULL, &address));
    assert(address);
    memcpy(destination, &address, sizeof(address));
}

int main(void)
{
    NewConfiguration create;
    DeleteConfiguration destroy;
    FromAssetManager from_manager;
    GetLocale language, country;
    GetInteger screen_size, width, height, nav_hidden;
    resolved("AConfiguration_new", &create);
    resolved("AConfiguration_delete", &destroy);
    resolved("AConfiguration_fromAssetManager", &from_manager);
    resolved("AConfiguration_getLanguage", &language);
    resolved("AConfiguration_getCountry", &country);
    resolved("AConfiguration_getScreenSize", &screen_size);
    resolved("AConfiguration_getScreenWidthDp", &width);
    resolved("AConfiguration_getScreenHeightDp", &height);
    resolved("AConfiguration_getNavHidden", &nav_hidden);

    uint64_t unresolved = 123;
    assert(!android_configuration_compat_resolve("AConfiguration_getDensity",
                                                 NULL, NULL, &unresolved));
    assert(!unresolved);
    assert(!android_configuration_compat_resolve("AConfiguration_new", "LIBC",
                                                 "libandroid.so", &unresolved));
    assert(!android_configuration_compat_resolve(NULL, NULL, NULL, &unresolved));
    assert(!android_configuration_compat_resolve("AConfiguration_new", NULL,
                                                 NULL, NULL));

    void *configuration = create();
    assert(configuration);
    char value[2] = {'x', 'x'};
    language(configuration, value);
    assert(value[0] == 0 && value[1] == 0);
    country(configuration, value);
    assert(value[0] == 0 && value[1] == 0);
    assert(screen_size(configuration) == 0 && width(configuration) == 0);
    assert(height(configuration) == 0 && nav_hidden(configuration) == 0);
    from_manager(configuration, NULL);
    assert(width(configuration) == 0); // Unregistered manager stays unknown.

    int java_manager = 1, env = 2;
    assert(android_asset_compat_bind_manager(&java_manager, "."));
    uint64_t address = 0;
    assert(android_asset_compat_resolve("AAssetManager_fromJava", NULL, NULL, &address));
    FromJava from_java = (FromJava)(uintptr_t)address;
    void *native_manager = from_java(&env, &java_manager);
    assert(native_manager);
    from_manager(configuration, native_manager);
    assert(width(configuration) == 0); // Directory does not imply Android dp.

    const AndroidAssetConfiguration supplied = {
        .language = {'e', 'n'}, .country = {'U', 'S'},
        .screen_size = 4, .screen_width_dp = 640,
        .screen_height_dp = 360, .nav_hidden = 2
    };
    assert(!android_asset_compat_set_configuration(NULL, &supplied));
    assert(!android_asset_compat_set_configuration(&java_manager, &supplied));
    assert(android_asset_compat_set_configuration(native_manager, &supplied));
    from_manager(configuration, native_manager);
    language(configuration, value);
    assert(value[0] == 'e' && value[1] == 'n');
    country(configuration, value);
    assert(value[0] == 'U' && value[1] == 'S');
    assert(screen_size(configuration) == 4);
    assert(width(configuration) == 640 && height(configuration) == 360);
    assert(nav_hidden(configuration) == 2);

    android_asset_compat_unbind_manager(&java_manager);
    AndroidAssetConfiguration returned;
    assert(!android_asset_compat_get_configuration(native_manager, &returned));
    assert(returned.screen_width_dp == 0);
    from_manager(configuration, native_manager);
    language(configuration, value);
    assert(!value[0] && !value[1]);
    assert(width(configuration) == 0); // Stale AAssetManager cannot leak metadata.
    destroy(configuration);
    puts("android_configuration_compat_test: nine imported NDK calls pass");
    return 0;
}
