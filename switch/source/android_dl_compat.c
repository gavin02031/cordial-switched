#define _POSIX_C_SOURCE 200809L
#include "android_dl_compat.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_log_compat.h"
#include "android_looper_compat.h"
#include "android_native_window_compat.h"
#include "android_asset_compat.h"
#include "android_configuration_compat.h"
#include "android_egl_gles_compat.h"
#include "android_math_compat.h"
#include "android_libc_compat.h"
#include "android_media_codec_compat.h"
#include "android_media_format_compat.h"
#include "android_image_reader_compat.h"
#include "engine_module_registry.h"

enum { DL_ERROR_BYTES = 128 };
enum { MODULE_LOG, MODULE_ANDROID, MODULE_MATH, MODULE_DL,
       MODULE_EGL, MODULE_GLES2, MODULE_LIBC, MODULE_MEDIANDK,
       MODULE_COUNT };

typedef struct {
    const char *name;
    unsigned opens;
} CompatModule;

static CompatModule modules[MODULE_COUNT] = {
    {"liblog.so", 0}, {"libandroid.so", 0},
    {"libm.so", 0}, {"libdl.so", 0},
    {"libEGL.so", 0}, {"libGLESv2.so", 0},
    {"libc.so", 0}, {"libmediandk.so", 0},
};
static pthread_mutex_t module_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char current[DL_ERROR_BYTES];
    char delivered[DL_ERROR_BYTES];
} DlErrorState;

static pthread_key_t error_key;
static pthread_once_t error_key_once = PTHREAD_ONCE_INIT;
static bool error_key_ready;

static void create_error_key(void)
{
    error_key_ready = pthread_key_create(&error_key, free) == 0;
}

static DlErrorState *thread_errors(void)
{
    if (pthread_once(&error_key_once, create_error_key) != 0 || !error_key_ready)
        return NULL;
    DlErrorState *state = pthread_getspecific(error_key);
    if (!state) {
        state = calloc(1, sizeof(*state));
        if (!state) return NULL;
        if (pthread_setspecific(error_key, state) != 0) {
            free(state);
            return NULL;
        }
    }
    return state;
}

static void set_error(const char *message)
{
    DlErrorState *state = thread_errors();
    if (state) {
        snprintf(state->current, sizeof(state->current), "%s", message);
    }
}

static char *bionic_dlerror(void)
{
    DlErrorState *state = thread_errors();
    if (!state || !state->current[0]) return NULL;
    memcpy(state->delivered, state->current, sizeof(state->delivered));
    state->current[0] = 0;
    return state->delivered;
}

static CompatModule *module_from_handle(void *handle)
{
    for (unsigned i = 0; i < MODULE_COUNT; i++) {
        if (handle == &modules[i]) return &modules[i];
    }
    return NULL;
}

static void *bionic_dlopen(const char *name, int flags)
{
    if (!name || !*name) {
        set_error("Android main-process dlopen is not implemented");
        return NULL;
    }
    const char *basename = strrchr(name, '/');
    if (basename) basename++; else basename = name;
    // Never replace an unknown absolute path by a same-named shim. Only
    // system/lib64 aliases map to the explicitly registered native adapters.
    if (basename != name && (strncmp(name, "/system/lib64/", 14) != 0 ||
                             basename != name + 14)) {
        set_error("External Android library path is unavailable");
        return NULL;
    }
    pthread_mutex_lock(&module_lock);
    for (unsigned i = 0; i < MODULE_COUNT; i++) {
        if (strcmp(basename, modules[i].name) == 0) {
            if ((i == MODULE_EGL || i == MODULE_GLES2) &&
                !android_egl_gles_compat_has_backend()) break;
            // Bionic RTLD_NOLOAD=4 only succeeds for already-open modules.
            if ((flags & 4) && modules[i].opens == 0) break;
            if (modules[i].opens == UINT32_MAX) break;
            modules[i].opens++;
            pthread_mutex_unlock(&module_lock);
            return &modules[i];
        }
    }
    pthread_mutex_unlock(&module_lock);
    set_error("Android shared library not loaded or not available in native bridge");
    return NULL;
}

static void *bionic_dlsym(void *handle, const char *name)
{
    if (!name || !*name) {
        set_error("Missing dynamic symbol name");
        return NULL;
    }
    CompatModule *module = module_from_handle(handle);
    if (!module) {
        set_error("Invalid or unsupported Android library handle");
        return NULL;
    }
    pthread_mutex_lock(&module_lock);
    const bool active = module->opens > 0;
    pthread_mutex_unlock(&module_lock);
    if (!active) {
        set_error("Android library handle was closed");
        return NULL;
    }
    uint64_t address = 0;
    if (module == &modules[MODULE_LOG])
        android_log_compat_resolve(name, &address, NULL);
    else if (module == &modules[MODULE_ANDROID]) {
        if (!android_looper_compat_resolve(name, NULL, NULL, &address) &&
            !android_native_window_compat_resolve(name, NULL, NULL, &address))
            if (!android_asset_compat_resolve(name, NULL, NULL, &address))
                android_configuration_compat_resolve(name, NULL, NULL, &address);
    }
    else if (module == &modules[MODULE_MATH])
        android_math_compat_resolve(name, "LIBC", "libm.so", &address);
    else if (module == &modules[MODULE_DL])
        android_dl_compat_resolve(name, "LIBC", "libdl.so", &address);
    else if (module == &modules[MODULE_LIBC])
        // Same symbols the static import resolver already serves for libc;
        // unknown names still get an honest NULL plus dlerror. Verified
        // guest path: dlopen("libc.so") then dlsym("getauxval") at
        // 0x1dd11d0, with a cbz fallback that handles either answer.
        android_libc_compat_resolve(name, "LIBC", "libc.so", &address);
    else if (module == &modules[MODULE_MEDIANDK]) {
        // libmediandk.so is DT_NEEDED in the guest, so Android semantics
        // make this dlopen succeed for the already-loaded library. Symbols
        // route to the same resolvers the static imports use; unavailable
        // codecs/images answer honestly at call time.
        if (!android_media_codec_compat_resolve(name, NULL, NULL, &address) &&
            !android_media_format_compat_resolve(name, NULL, NULL, &address))
            android_image_reader_compat_resolve(name, NULL, NULL, &address);
    }
    else if ((module == &modules[MODULE_EGL] && strncmp(name, "egl", 3) == 0) ||
             (module == &modules[MODULE_GLES2] && strncmp(name, "gl", 2) == 0))
        android_egl_gles_compat_resolve(name, NULL, NULL, &address);
    if (!address) {
        set_error("Requested Android symbol is not implemented");
        return NULL;
    }
    return (void *)(uintptr_t)address;
}

static int bionic_dlclose(void *handle)
{
    CompatModule *module = module_from_handle(handle);
    if (!module) {
        set_error("Invalid Android library handle");
        return -1;
    }
    pthread_mutex_lock(&module_lock);
    if (!module->opens) {
        pthread_mutex_unlock(&module_lock);
        set_error("Android library handle was already closed");
        return -1;
    }
    module->opens--;
    pthread_mutex_unlock(&module_lock);
    return 0;
}

static int bionic_dladdr(const void *address, void *info)
{
    // A valid Dl_info requires real guest module base/symbol ownership. A
    // native callback pointer alone is not evidence of an Android ELF DSO.
    (void)address;
    (void)info;
    return 0;
}

// Bionic LP64 dl_phdr_info: the classic four fields, then the load-counter
// and TLS fields added in later Android releases. Handing the guest's
// callback the full modern layout keeps both old and new consumers inside
// the struct they were compiled against. The TLS module id is 0 until the
// guest TLS registry can supply a real one; unwinders, the known callers,
// read only the classic prefix.
typedef struct {
    uint64_t addr;        // dlpi_addr: load bias, not the first mapped byte.
    const char *name;     // dlpi_name
    const void *phdr;     // dlpi_phdr
    uint16_t phnum;       // dlpi_phnum
    uint64_t adds;        // dlpi_adds
    uint64_t subs;        // dlpi_subs
    uint64_t tls_modid;   // dlpi_tls_modid (0: no id assigned yet)
    void *tls_data;       // dlpi_tls_data
} BionicDlPhdrInfo;

static int bionic_dl_iterate_phdr(int (*callback)(void *, size_t, void *),
                                  void *data)
{
    if (!callback) {
        set_error("Missing dl_iterate_phdr callback");
        return -1;
    }
    // Snapshot first: the callback may itself call dl_iterate_phdr or
    // dlopen, and the registry lock is not recursive. An empty walk means
    // the loader has not mapped a guest image yet - a true statement about
    // Morrow's own state, never a claim that a missing module is loaded.
    EngineModule found[ENGINE_MODULE_REGISTRY_MAX];
    unsigned count = engine_module_snapshot(found, ENGINE_MODULE_REGISTRY_MAX);
    for (unsigned i = 0; i < count; i++) {
        BionicDlPhdrInfo info;
        memset(&info, 0, sizeof(info));
        info.addr = found[i].load_bias;
        info.name = found[i].name;
        info.phdr = found[i].program_headers;
        info.phnum = found[i].program_header_count;
        info.adds = engine_module_adds();
        info.subs = engine_module_subs();
        int stop = callback(&info, sizeof(info), data);
        if (stop) return stop;
    }
    return 0;
}

bool android_dl_compat_resolve(const char *name, const char *version,
                               const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libdl.so") != 0)
        return false;
    *address = 0;
#define DL_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    DL_MAP(dlopen, bionic_dlopen)
    DL_MAP(dlsym, bionic_dlsym)
    DL_MAP(dlclose, bionic_dlclose)
    DL_MAP(dlerror, bionic_dlerror)
    DL_MAP(dladdr, bionic_dladdr)
    DL_MAP(dl_iterate_phdr, bionic_dl_iterate_phdr)
    (void)0;
#undef DL_MAP
    return *address != 0;
}
