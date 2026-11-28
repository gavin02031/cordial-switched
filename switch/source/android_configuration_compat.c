#include "android_configuration_compat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "android_asset_compat.h"

typedef struct {
    AndroidAssetConfiguration values;
} GuestConfiguration;

static GuestConfiguration *new_configuration(void)
{
    // Zero is Android's ACONFIGURATION_*_ANY for the imported fields; a
    // missing JNI AssetManager cannot authoritatively supply locale or dp.
    return calloc(1, sizeof(GuestConfiguration));
}

static void delete_configuration(GuestConfiguration *configuration)
{
    free(configuration);
}

static void from_asset_manager(GuestConfiguration *configuration, void *manager)
{
    if (!configuration) return;
    AndroidAssetConfiguration supplied = {0};
    // Clear even if the manager has since been unbound. A recycled Java
    // reference must not inherit another application's locale and layout.
    android_asset_compat_get_configuration(manager, &supplied);
    configuration->values = supplied;
}

static void get_language(GuestConfiguration *configuration, char out[2])
{
    if (!out) return;
    const AndroidAssetConfiguration *data = configuration ? &configuration->values : NULL;
    out[0] = data ? data->language[0] : 0;
    out[1] = data ? data->language[1] : 0;
}

static void get_country(GuestConfiguration *configuration, char out[2])
{
    if (!out) return;
    const AndroidAssetConfiguration *data = configuration ? &configuration->values : NULL;
    out[0] = data ? data->country[0] : 0;
    out[1] = data ? data->country[1] : 0;
}

static int32_t get_screen_size(GuestConfiguration *configuration)
{
    return configuration ? configuration->values.screen_size : 0;
}

static int32_t get_screen_width_dp(GuestConfiguration *configuration)
{
    return configuration ? configuration->values.screen_width_dp : 0;
}

static int32_t get_screen_height_dp(GuestConfiguration *configuration)
{
    return configuration ? configuration->values.screen_height_dp : 0;
}

static int32_t get_nav_hidden(GuestConfiguration *configuration)
{
    return configuration ? configuration->values.nav_hidden : 0;
}

bool android_configuration_compat_resolve(const char *name, const char *version,
                                           const char *library, uint64_t *address)
{
    if (!name || !address || version || library) return false;
    *address = 0;
#define CONFIG_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    CONFIG_MAP(AConfiguration_new, new_configuration)
    CONFIG_MAP(AConfiguration_delete, delete_configuration)
    CONFIG_MAP(AConfiguration_fromAssetManager, from_asset_manager)
    CONFIG_MAP(AConfiguration_getLanguage, get_language)
    CONFIG_MAP(AConfiguration_getCountry, get_country)
    CONFIG_MAP(AConfiguration_getScreenSize, get_screen_size)
    CONFIG_MAP(AConfiguration_getScreenWidthDp, get_screen_width_dp)
    CONFIG_MAP(AConfiguration_getScreenHeightDp, get_screen_height_dp)
    CONFIG_MAP(AConfiguration_getNavHidden, get_nav_hidden)
    (void)0;
#undef CONFIG_MAP
    return *address != 0;
}
