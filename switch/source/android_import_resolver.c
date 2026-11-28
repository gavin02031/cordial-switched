#include "android_import_resolver.h"
#include <stddef.h>
#include "android_log_compat.h"
#include "android_media_codec_compat.h"
#include "android_image_reader_compat.h"
#include "android_coverage_compat.h"
#include "android_media_format_compat.h"
#include "android_cxa_compat.h"
#include "android_looper_compat.h"
#include "android_native_window_compat.h"
#include "android_asset_compat.h"
#include "android_configuration_compat.h"
#include "android_egl_gles_compat.h"
#include "android_dl_compat.h"
#include "android_math_compat.h"
#include "android_jump_compat.h"
#include "android_libc_compat.h"
#include "android_process_compat.h"
#include "android_system_compat.h"
#include "android_wchar_compat.h"
#include "android_socket_compat.h"
#include "android_file_compat.h"
#include "android_mapping_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"
#include "android_pthread_compat.h"
#include "android_sem_compat.h"
#include "android_tls_compat.h"
#include "android_thread_attr_compat.h"
#include "android_thread_lifecycle_compat.h"

// The preflight and final-base linker must ask the same compiled providers.
// Platform-dependent providers still decide availability at runtime.
bool android_import_resolve(const char *name, const char *version,
                            const char *library, uint64_t *address)
{
    if (!name || !address) return false;
    *address = 0;
    return ((!version && !library &&
           android_log_compat_resolve(name, address, NULL)) ||
           android_media_codec_compat_resolve(name, version, library, address) ||
           android_image_reader_compat_resolve(name, version, library, address) ||
           android_coverage_compat_resolve(name, version, library, address) ||
           android_media_format_compat_resolve(name, version, library, address) ||
           android_cxa_compat_resolve(name, version, library, address) ||
           android_looper_compat_resolve(name, version, library, address) ||
           android_native_window_compat_resolve(name, version, library, address) ||
           android_asset_compat_resolve(name, version, library, address) ||
           android_configuration_compat_resolve(name, version, library, address) ||
           android_egl_gles_compat_resolve(name, version, library, address) ||
           android_dl_compat_resolve(name, version, library, address) ||
           android_math_compat_resolve(name, version, library, address) ||
           android_jump_compat_resolve(name, version, library, address) ||
           android_libc_compat_resolve(name, version, library, address) ||
           android_process_compat_resolve(name, version, library, address) ||
           android_system_compat_resolve(name, version, library, address) ||
           android_wchar_compat_resolve(name, version, library, address) ||
           android_socket_compat_resolve(name, version, library, address) ||
           android_file_compat_resolve(name, version, library, address) ||
           android_mapping_compat_resolve(name, version, library, address) ||
           android_stdio_compat_resolve(name, version, library, address) ||
           android_fortify_compat_resolve(name, version, library, address) ||
           android_pthread_compat_resolve(name, version, library, address) ||
           android_sem_compat_resolve(name, version, library, address) ||
           android_tls_compat_resolve(name, version, library, address) ||
           android_thread_attr_compat_resolve(name, version, library, address) ||
           android_thread_lifecycle_compat_resolve(name, version, library, address));
}
