#include "horizon_real_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "android_log_compat.h"
#include "android_media_codec_compat.h"
#include "android_cxa_compat.h"
#include "android_looper_compat.h"
#include "android_asset_compat.h"
#include "android_configuration_compat.h"
#include "android_egl_gles_compat.h"
#include "android_jni_vm_bridge.h"
#ifdef MORROW_HAS_JNIVM
#include "morrow_jni_runtime.h"
#endif
#include "android_native_window_compat.h"
#include "android_dl_compat.h"
#include "android_media_format_compat.h"
#include "android_math_compat.h"
#include "android_libc_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"
#include "android_pthread_compat.h"
#include "android_tls_compat.h"
#include "android_thread_attr_compat.h"
#include "android_thread_lifecycle_compat.h"
#include "elf_probe.h"
#include "engine_entry_probe.h"
#include "engine_execution_attempt.h"
#include "engine_memory_plan.h"
#include "engine_stage.h"
#include "engine_tls_scratch.h"
#include "engine_tls_thread.h"
#include "horizon_code_address.h"
#include "horizon_code_memory.h"

static HorizonCodeMemory reservation;
// JNI_OnLoad can register callbacks whose function pointers live in the
// mapped library. After entering it, neither the RX mapping nor its source
// can safely be unloaded by a second L3; the OS reclaims both on app exit.
static bool engine_entry_started;
static bool engine_mapping_pinned;
static bool engine_jni_onload_succeeded;
static bool game_activity_attempt_consumed;
static bool game_activity_entry_called;
static uintptr_t live_game_activity_target;

static const char *reserve_status_name(HorizonCodeReserveStatus status)
{
    switch (status) {
    case HORIZON_RESERVE_OK: return "ok";
    case HORIZON_RESERVE_INVALID_PLAN: return "invalid_memory_plan";
    case HORIZON_RESERVE_SYSCALL_UNAVAILABLE: return "required_syscall_unavailable";
    case HORIZON_RESERVE_NO_FREE_SPAN: return "no_contiguous_code_address_span";
    case HORIZON_RESERVE_NO_RESERVATION_MEMORY: return "reservation_allocation_failed";
    case HORIZON_RESERVE_INVALID_STATE: return "reservation_already_active";
    }
    return "unknown";
}

static const char *entry_status_name(EngineEntryProbeStatus status)
{
    switch (status) {
    case ENGINE_ENTRY_INVALID: return "invalid_elf_metadata";
    case ENGINE_ENTRY_IO_ERROR: return "io_error";
    case ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS: return "export_symbols_unknown";
    case ENGINE_ENTRY_INSPECTED: return "export_symbols_inspected";
    }
    return "unknown";
}

typedef struct {
    char name[80];
    char version[48];
    char library[48];
} MissingImport;

typedef struct {
    MissingImport first;
    MissingImport examples[24];
    unsigned lookups;
    unsigned supported;
    unsigned missing;
} ImportDiagnostics;

static FILE *new_log(char *path, size_t capacity)
{
    for (unsigned i = 1; i <= 99; i++) {
        const int n = snprintf(path, capacity,
                               ENGINE_REPORT_DIR "/link-%02u.txt", i);
        if (n <= 0 || (size_t)n >= capacity) break;
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        FILE *stream = fdopen(fd, "w");
        if (stream) return stream;
        close(fd);
        remove(path);
        break;
    }
    path[0] = 0;
    return NULL;
}
static void log_line(FILE *out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    fflush(out);
}
static void log_engine_export(FILE *log, const char *name,
                              const EngineEntryProbe *entry,
                              bool found, uint64_t virtual_address)
{
    log_line(log, "%s_exported=%s", name,
             entry->status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             found ? "yes" : "no");
    if (entry->status == ENGINE_ENTRY_INSPECTED && found)
        log_line(log, "%s_virtual_address=0x%llx", name,
                 (unsigned long long)virtual_address);
}
static void detail(RealLinkResult *result, const char *message)
{
    snprintf(result->detail, sizeof(result->detail), "%.110s", message);
}
static bool resolve_compatible(const char *name, const char *version,
                               const char *library, uint64_t *address,
                               void *context)
{
    ImportDiagnostics *diagnostics = context;
    if (diagnostics && diagnostics->lookups < UINT32_MAX)
        diagnostics->lookups++;
    // Match the same real providers used by ZR's import audit, without
    // fabricating pointers for Android services that do not exist on Horizon.
    const bool supported = ((!version && !library &&
             android_log_compat_resolve(name, address, NULL)) ||
            android_media_codec_compat_resolve(name, version, library, address) ||
            android_media_format_compat_resolve(name, version, library, address) ||
            android_cxa_compat_resolve(name, version, library, address) ||
            android_looper_compat_resolve(name, version, library, address) ||
            android_native_window_compat_resolve(name, version, library, address) ||
            android_asset_compat_resolve(name, version, library, address) ||
            android_configuration_compat_resolve(name, version, library, address) ||
            android_egl_gles_compat_resolve(name, version, library, address) ||
            android_dl_compat_resolve(name, version, library, address) ||
            android_math_compat_resolve(name, version, library, address) ||
            android_libc_compat_resolve(name, version, library, address) ||
            android_stdio_compat_resolve(name, version, library, address) ||
            android_fortify_compat_resolve(name, version, library, address) ||
            android_pthread_compat_resolve(name, version, library, address) ||
            android_tls_compat_resolve(name, version, library, address) ||
            android_thread_attr_compat_resolve(name, version, library, address) ||
            android_thread_lifecycle_compat_resolve(name, version, library, address));
    if (supported) {
        if (diagnostics && diagnostics->supported < UINT32_MAX)
            diagnostics->supported++;
    } else if (diagnostics) {
        MissingImport value = {0};
        // Match symbol names to their version and owning DSO; a same-named
        // libc function does not automatically implement its Android ABI.
        snprintf(value.name, sizeof(value.name), "%.75s", name ? name : "?");
        snprintf(value.version, sizeof(value.version), "%.44s",
                 version ? version : "unversioned");
        snprintf(value.library, sizeof(value.library), "%.44s",
                 library ? library : "unspecified");
        if (!diagnostics->first.name[0]) diagnostics->first = value;
        if (diagnostics->missing < 24)
            diagnostics->examples[diagnostics->missing] = value;
        if (diagnostics->missing < UINT32_MAX) diagnostics->missing++;
    }
    return supported;
}
static void printable(char *out, size_t size, const char *message)
{
    size_t i = 0;
    while (message && message[i] && i + 1 < size) {
        const unsigned char ch = (unsigned char)message[i];
        out[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        i++;
    }
    out[i] = 0;
}

static void log_import_summary(FILE *log, const char *prefix,
                               const ImportDiagnostics *diagnostics)
{
    log_line(log, "%s_import_lookups=%u", prefix, diagnostics->lookups);
    log_line(log, "%s_supported_imports=%u", prefix, diagnostics->supported);
    log_line(log, "%s_missing_imports=%u", prefix, diagnostics->missing);
    if (diagnostics->first.name[0]) {
        char name[80], version[48], library[48];
        printable(name, sizeof(name), diagnostics->first.name);
        printable(version, sizeof(version), diagnostics->first.version);
        printable(library, sizeof(library), diagnostics->first.library);
        log_line(log, "%s_first_missing_name=%s", prefix, name);
        log_line(log, "%s_first_missing_version=%s", prefix, version);
        log_line(log, "%s_first_missing_provider=%s", prefix, library);
    }
    for (unsigned i = 0; i < diagnostics->missing && i < 24; i++) {
        char name[80], version[48], library[48];
        printable(name, sizeof(name), diagnostics->examples[i].name);
        printable(version, sizeof(version), diagnostics->examples[i].version);
        printable(library, sizeof(library), diagnostics->examples[i].library);
        log_line(log, "%s_missing_import_%02u=%s @ %s from %s",
                 prefix, i + 1, name, version, library);
    }
}

static void audit_thread_tls(FILE *library, const EngineProbeResult *profile,
                             FILE *log)
{
    EngineTlsScratch template_image = {0};
    const EngineTlsScratchStatus status =
        engine_tls_scratch_prepare(library, profile, &template_image);
    log_line(log, "tls_template_status=%d", status);
    log_line(log, "tls_thread_pointer_installed=no");
    log_line(log, "tls_dynamic_modules_supported=no");
    if (status == ENGINE_TLS_PREPARED && template_image.size) {
        EngineTlsThread thread = {0};
        const bool copied = engine_tls_thread_prepare(&template_image, NULL, &thread);
        log_line(log, "tls_thread_image_prepared=%s", copied ? "yes" : "no");
        if (copied) {
            log_line(log, "tls_thread_image_bytes=%zu", thread.size);
            log_line(log, "tls_thread_image_alignment=%zu", thread.alignment);
            log_line(log, "tls_thread_image_is_private=%s",
                     thread.data != template_image.data ? "yes" : "no");
            log_line(log, "tls_dtv_slot_1_prepared=%s",
                     thread.tcb.dtv == &thread.dtv &&
                     thread.dtv.modules[1] == thread.data ? "yes" : "no");
        }
        engine_tls_thread_release(&thread);
    } else if (status == ENGINE_TLS_PREPARED) {
        log_line(log, "tls_thread_image_prepared=empty_template");
    } else {
        log_line(log, "tls_thread_image_prepared=no");
    }
    engine_tls_scratch_release(&template_image);
}

void horizon_real_link_run(RealLinkResult *result, unsigned action)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->attempted = true;
    if (mkdir(ENGINE_REPORT_DIR, 0777) != 0 && errno != EEXIST) {
        detail(result, "Cannot create the SD diagnostic folder.");
        return;
    }
    FILE *log = new_log(result->log_path, sizeof(result->log_path));
    if (!log) {
        detail(result, "Cannot save a numbered link report.");
        return;
    }
    log_line(log, "# Morrow native engine entry readiness attempt");
    log_line(log, "# JNI_OnLoad is opt-in and blocked unless strict mapping and runtime gates pass.");
    log_line(log, "engine_execution_action=%s",
             action == 1 ? "jni_onload" : action == 2 ? "game_activity" : "diagnostic");
    if (action == 2) {
        log_line(log, "game_activity_prior_jni_onload_succeeded=%s",
                 engine_jni_onload_succeeded ? "yes" : "no");
        log_line(log, "game_activity_entry_pointer_valid=%s",
                 live_game_activity_target ? "yes" : "no");
        if (!engine_mapping_pinned || !engine_jni_onload_succeeded ||
            !live_game_activity_target) {
            detail(result, "First complete ZL+L3 JNI_OnLoad before GameActivity.");
            log_line(log, "blocker=engine_jni_onload_not_ready");
            goto done;
        }
        if (game_activity_attempt_consumed) {
            detail(result, "Activity entry already attempted. Restart Morrow.");
            log_line(log, "blocker=game_activity_already_attempted");
            goto done;
        }
#ifdef MORROW_HAS_JNIVM
        const bool env_ready = morrow_jni_runtime_jni_env() != NULL;
        const bool activity_ready = morrow_jni_runtime_game_activity_ready();
        const bool config_ready = morrow_jni_runtime_configuration_bound();
        const bool assets_ready = morrow_jni_runtime_asset_manager_bound();
        const bool surface_ready = morrow_jni_runtime_surface_bound();
        log_line(log, "activity_jni_env_ready=%s", env_ready ? "yes" : "no");
        log_line(log, "activity_java_object_ready=%s", activity_ready ? "yes" : "no");
        log_line(log, "activity_configuration_ready=%s", config_ready ? "yes" : "no");
        log_line(log, "activity_real_asset_manager_ready=%s", assets_ready ? "yes" : "no");
        log_line(log, "activity_horizon_surface_ready=%s", surface_ready ? "yes" : "no");
        if (!env_ready || !activity_ready || !config_ready ||
            !assets_ready || !surface_ready) {
            detail(result, "GameActivity blocked: missing actual JNI/assets/surface.");
            log_line(log, "blocker=game_activity_jni_prerequisite_missing");
            goto done;
        }
        game_activity_attempt_consumed = true;
        MorrowGameActivityInitResult init = {0};
        log_line(log, "event=entering_real_game_activity_initialize_native");
        detail(result, "GameActivity attempted. Inspect link report; restart to retry.");
        const bool returned = morrow_jni_runtime_call_game_activity(
            live_game_activity_target, ENGINE_REPORT_DIR, &init);
        game_activity_entry_called = init.entry_invoked;
        log_line(log, "game_activity_initializer_returned=%s",
                 returned && init.entry_returned ? "yes" : "no");
        log_line(log, "game_activity_jni_exception_pending=%s",
                 init.java_exception_pending ? "yes" : "no");
        if (returned)
            log_line(log, "game_activity_native_handle=0x%llx",
                     (unsigned long long)init.native_handle);
        log_line(log, "engine_gameplay_started=no");
#else
        detail(result, "JNI-enabled Morrow artifact required for GameActivity.");
        log_line(log, "blocker=jni_runtime_not_compiled");
#endif
        goto done;
    }
    if (engine_mapping_pinned) {
        detail(result, "Engine code mapping held. Restart Morrow before retrying.");
        log_line(log, "blocker=prior_engine_mapping_requires_process_restart");
        goto done;
    }
    bool java_surface_bound = false;
#ifdef MORROW_HAS_JNIVM
    java_surface_bound = morrow_jni_runtime_surface_bound();
#endif
    log_line(log, "android_java_surface_registered=%s",
             java_surface_bound ? "yes" : "no");
    log_line(log, "android_java_surface_alias_resolution=%s",
             java_surface_bound ? "known_vm_references_only" : "unavailable");
    log_line(log, "android_java_surface_used_by_engine=no");
    bool java_assets_bound = false;
#ifdef MORROW_HAS_JNIVM
    java_assets_bound = morrow_jni_runtime_asset_manager_bound();
#endif
    log_line(log, "android_java_asset_manager_registered=%s",
             java_assets_bound ? "yes" : "no");
    bool java_configuration_bound = false;
#ifdef MORROW_HAS_JNIVM
    java_configuration_bound = morrow_jni_runtime_configuration_bound();
#endif
    log_line(log, "jni_java_configuration_object_registered=%s",
             java_configuration_bound ? "yes" : "no");
    log_line(log, "jni_java_configuration_primitive_fields=%s",
             java_configuration_bound ?
             (java_surface_bound ? "18_registered_viewport_1x_layout" :
                                   "18_registered_no_surface_geometry") : "unavailable");
    log_line(log, "jni_java_configuration_locales=%s",
             java_configuration_bound ? "device_locale_if_available" : "unavailable");
    log_line(log, "android_java_asset_manager_root=optional_user_assets");
    log_line(log, "android_asset_configuration_registered=no");
    log_line(log, "android_configuration_ndk_functions=9");
    log_line(log, "jni_vm_invocation_abi=%s",
             android_jni_vm_bridge_instance() ? "available" : "unavailable");
    const bool jni_provider_ready = android_jni_vm_bridge_has_provider();
    log_line(log, "jni_vm_provider_installed=%s", jni_provider_ready ? "yes" : "no");
    log_line(log, "jni_env_provider_connected=%s", jni_provider_ready ? "yes" : "no");
    bool synthetic_onload_passed = false;
    bool synthetic_vm_available = false;
    bool synthetic_entry_accepted = false;
    bool synthetic_native_invoked = false;
    bool synthetic_cleanup = false;
#ifdef MORROW_HAS_JNIVM
    MorrowJniOnLoadProbe synthetic_onload = {0};
    synthetic_onload_passed = morrow_jni_runtime_probe_onload(&synthetic_onload);
    synthetic_vm_available = synthetic_onload.vm_available;
    synthetic_entry_accepted = synthetic_onload.jni_onload_accepted;
    synthetic_native_invoked = synthetic_onload.native_invoked;
    synthetic_cleanup = synthetic_onload.native_unregistered &&
                        synthetic_onload.local_references_released;
#endif
    const char *synthetic_status = !synthetic_vm_available ? "vm_unavailable" :
                                   !synthetic_onload_passed ? "registration_or_cleanup_failed" :
                                   !synthetic_native_invoked ? "native_dispatch_missing" : "passed";
    log_line(log, "morrow_synthetic_jni_onload_probe=%s", synthetic_status);
    log_line(log, "morrow_synthetic_jni_onload_vm_available=%s",
             synthetic_vm_available ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_entry_accepted=%s",
             synthetic_entry_accepted ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_native_invoked=%s",
             synthetic_native_invoked ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_cleanup=%s",
             synthetic_cleanup ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_roblox_code_executed=no");
    log_line(log, "jni_runtime_hardware_verified=no");
    log_line(log, "jni_engine_class_callbacks_complete=no");
    bool game_activity_ready = false;
#ifdef MORROW_HAS_JNIVM
    game_activity_ready = morrow_jni_runtime_game_activity_ready();
#endif
    log_line(log, "jni_game_activity_object_registered=%s",
             game_activity_ready ? "yes" : "no");
#ifdef MORROW_HAS_JNIVM
    log_line(log, "jni_system_locale_bound=%s",
             morrow_jni_runtime_system_locale_bound() ? "yes" : "no");
    int64_t sd_bytes = 0;
    const bool sd_measured = morrow_jni_runtime_storage_available_bytes(&sd_bytes);
    log_line(log, "jni_storage_free_space_measured=%s",
             sd_measured ? "yes" : "no");
    if (sd_measured)
        log_line(log, "jni_storage_available_bytes=%lld", (long long)sd_bytes);
    log_line(log, "jni_local_storage_manager_registered=%s",
             morrow_jni_runtime_local_storage_manager() ? "yes" : "no");
#else
    log_line(log, "jni_system_locale_bound=no");
    log_line(log, "jni_storage_free_space_measured=no");
    log_line(log, "jni_local_storage_manager_registered=no");
#endif
    log_line(log, "roblox_storage_initialised=no");
    log_line(log, "jni_game_activity_class_loader=%s",
             game_activity_ready ? "registered_classes_only" : "unavailable");
    log_line(log, "jni_apk_classpath_installed=no");
    bool partial_app_params = false;
    bool partial_init_params = false;
#ifdef MORROW_HAS_JNIVM
    partial_app_params =
        morrow_jni_runtime_app_bridge_start_params() != NULL &&
        morrow_jni_runtime_app_bridge_start_params_bound();
    partial_init_params =
        morrow_jni_runtime_app_bridge_init_params() != NULL &&
        morrow_jni_runtime_app_bridge_init_params_bound();
#endif
    log_line(log, "app_bridge_init_params_typed_partial=%s",
             partial_init_params ? "yes" : "no");
    log_line(log, "app_bridge_init_params_device_fields=%s",
             partial_init_params ? "21_typed_some_values_unknown" : "unavailable");
    log_line(log, "app_bridge_init_params_user_agent=unknown_unset");
    log_line(log, "app_bridge_init_params_android_api=unknown_unset");
    log_line(log, "app_bridge_start_app_params_typed_partial=%s",
             partial_app_params ? "yes" : "no");
    log_line(log, "app_bridge_start_app_params_surface_from_horizon=%s",
             partial_app_params ? "yes" : "no");
    log_line(log, "app_bridge_start_app_params_platform_assets=%s",
             partial_app_params ? "bound_read_only_directory" : "unavailable");
    log_line(log, "app_bridge_init_params_complete=no");
    log_line(log, "app_bridge_android_app_callbacks_complete=no");
    log_line(log, "app_bridge_account_login_available=no");
    log_line(log, "jni_game_activity_insets_callback=%s",
             game_activity_ready ? "zero_fullscreen_insets_only" : "unavailable");
    log_line(log, "jni_game_activity_get_assets_callback=%s",
             game_activity_ready ? "bound_read_only_manager_or_null" : "unavailable");
    log_line(log, "jni_game_activity_get_resources_callback=%s",
             game_activity_ready ? "measured_horizon_surface_or_null" : "unavailable");
    log_line(log, "jni_resources_get_configuration_callback=%s",
             java_configuration_bound && java_surface_bound ?
             "live_viewport_configuration" : "null_until_both_bound");
    log_line(log, "jni_display_metrics_density=neutral_1x_not_physical_dpi");
    log_line(log, "jni_game_activity_window_ime_callbacks=%s",
             game_activity_ready ? "registered_observation_only" : "unavailable");
    uint32_t window_flag_requests = 0;
    uint32_t ime_editor_info_requests = 0;
#ifdef MORROW_HAS_JNIVM
    MorrowGameActivityRequests activity_requests = {0};
    if (morrow_jni_runtime_activity_requests(&activity_requests)) {
        window_flag_requests = activity_requests.window_flag_requests;
        ime_editor_info_requests = activity_requests.ime_editor_info_requests;
    }
#endif
    log_line(log, "jni_game_activity_window_flag_request_count=%u", window_flag_requests);
    log_line(log, "jni_game_activity_ime_editor_info_request_count=%u", ime_editor_info_requests);
    log_line(log, "jni_game_activity_window_flags_applied=no");
    log_line(log, "jni_game_activity_ime_editor_info_applied=no");
    log_line(log, "jni_asset_reference_alias_resolution=%s",
             jni_provider_ready ? "known_vm_references_only" : "unavailable");
    log_line(log, "jni_game_activity_bootstrap_implemented=no");
    log_line(log, "jni_onload_call_eligible=requires_runtime_preflight");
    log_line(log, "native_egl_gles_symbols_linked=%s",
             android_egl_gles_compat_has_backend() ? "yes" : "no");
    log_line(log, "native_egl_gles_symbol_catalogue=%u",
             android_egl_gles_compat_known_count());
    log_line(log, "guest_egl_window_surface_bridge=registered_surfaces_only");
    log_line(log, "guest_egl_window_surface_registered=no");
    log_line(log, "guest_egl_context_current=no");
    log_line(log, "event=preflight_started");

    // On a rare release failure retain the reservation for the next L3.
    Result kernel_error = 0;
    if (reservation.reservation &&
        !horizon_code_memory_unmap(&reservation, &kernel_error)) {
        detail(result, "Previous code-address reservation could not be released.");
        log_line(log, "blocker=previous_reservation_cleanup_failed");
        goto done;
    }

    FILE *file = fopen(ENGINE_LIBRARY_PATH, "rb");
    if (!file) {
        detail(result, "No ARM64 libroblox.so in the SD engine folder.");
        log_line(log, "blocker=engine_file_missing");
        goto done;
    }

    EngineProbeResult probe;
    engine_probe_stream(file, &probe);
    log_line(log, "inspect_status=%d", probe.status);
    if (probe.status != ENGINE_PROBE_INSPECTED) {
        detail(result, probe.detail);
        log_line(log, "blocker=elf_inspection_failed");
        fclose(file);
        goto done;
    }
    log_line(log, "file_bytes=%llu", (unsigned long long)probe.file_bytes);
    log_line(log, "load_segments=%u", probe.load_segments);
    log_line(log, "max_segment_alignment=%llu",
             (unsigned long long)probe.max_segment_alignment);
    log_line(log, "android_dependencies=%u", probe.needed_count);
    log_line(log, "tls_segment=%s", probe.has_tls_segment ? "present" : "absent");
    audit_thread_tls(file, &probe, log);

    EngineEntryProbe entry = {0};
    engine_entry_probe_stream(file, &entry);
    log_line(log, "entry_probe_status=%d", entry.status);
    log_line(log, "entry_probe_status_name=%s", entry_status_name(entry.status));
    char entry_detail[sizeof(entry.detail)];
    printable(entry_detail, sizeof(entry_detail), entry.detail);
    log_line(log, "entry_probe_detail=%s", entry_detail);
    log_line(log, "jni_onload_exported=%s",
             entry.status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             entry.has_jni_onload ? "yes" : "no");
    if (entry.status == ENGINE_ENTRY_INSPECTED && entry.has_jni_onload)
        log_line(log, "jni_onload_virtual_address=0x%llx",
                 (unsigned long long)entry.jni_onload_virtual_address);
    log_line(log, "game_activity_initialize_native_exported=%s",
             entry.status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             entry.has_game_activity_initialize_native ? "yes" : "no");
    if (entry.status == ENGINE_ENTRY_INSPECTED &&
        entry.has_game_activity_initialize_native)
        log_line(log, "game_activity_initialize_native_virtual_address=0x%llx",
                 (unsigned long long)entry.game_activity_initialize_native_virtual_address);
    log_line(log, "game_activity_initialize_native_call_eligible=only_after_successful_jni_onload");
    log_line(log, "native_activity_exported=%s",
             entry.status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             entry.has_native_activity_oncreate ? "yes" : "no");
    if (entry.status == ENGINE_ENTRY_INSPECTED && entry.has_native_activity_oncreate)
        log_line(log, "native_activity_virtual_address=0x%llx",
                 (unsigned long long)entry.native_activity_virtual_address);
    // Cordial's Android launch trace and app-bridge investigation agree that
    // the Roblox home shell uses NativeGLInterface's V2 bridge, not AGDK's
    // GameActivity surface callbacks. Inspect these actual exports before
    // attempting another ABI-dependent guest entry point.
    log_engine_export(log, "app_bridge_global_init", &entry,
                      entry.has_app_bridge_global_init,
                      entry.app_bridge_global_init_virtual_address);
    log_engine_export(log, "app_bridge_v2_init", &entry,
                      entry.has_app_bridge_v2_init,
                      entry.app_bridge_v2_init_virtual_address);
    log_engine_export(log, "app_bridge_lua_shell_start", &entry,
                      entry.has_app_bridge_lua_shell_start,
                      entry.app_bridge_lua_shell_start_virtual_address);
    log_engine_export(log, "app_bridge_start_app", &entry,
                      entry.has_app_bridge_start_app,
                      entry.app_bridge_start_app_virtual_address);
    log_line(log, "app_bridge_startup_call_eligible=no_missing_real_autovalue_params_and_lifecycle");
    log_line(log, "app_bridge_startup_called=no");
    log_line(log, "elf_dt_init=%s", entry.has_init ? "present" : "absent");
    if (entry.has_init)
        log_line(log, "elf_dt_init_virtual_address=0x%llx",
                 (unsigned long long)entry.init_virtual_address);
    log_line(log, "elf_init_array_entries=%u", entry.init_array_entries);
    log_line(log, "engine_initializer_called=no");

    EngineMemoryPlan plan = {0};
    if (!engine_memory_plan_from_probe(&probe, &plan)) {
        detail(result, "Cannot construct safe Horizon RX/R/RW plan.");
        log_line(log, "blocker=memory_plan_invalid");
        fclose(file);
        goto done;
    }
    log_line(log, "virtual_image_bytes=%zu", plan.image_bytes);
    size_t search_bytes = 0;
    if (horizon_code_address_search_size(&plan, &search_bytes))
        log_line(log, "code_address_search_bytes=%zu", search_bytes);
    log_line(log, "code_address_alignment_phase=%llu",
             (unsigned long long)(plan.virtual_page_base % plan.alignment_bytes));
    log_line(log, "event=reserve_final_address");
    HorizonCodeReserveStatus reserve_status = HORIZON_RESERVE_INVALID_STATE;
    if (!horizon_code_memory_reserve_detailed(&plan, &reservation, &reserve_status)) {
        if (reserve_status == HORIZON_RESERVE_NO_FREE_SPAN)
            detail(result, "No contiguous code address found. Try title override / restart.");
        else if (reserve_status == HORIZON_RESERVE_SYSCALL_UNAVAILABLE)
            detail(result, "Horizon code-memory syscalls unavailable in this launch mode.");
        else
            detail(result, "Horizon could not reserve a final code address.");
        log_line(log, "reserve_failure_reason=%s", reserve_status_name(reserve_status));
        log_line(log, "blocker=final_address_reservation_failed");
    } else {
        log_line(log, "event=final_address_reserved");
        log_line(log, "reserved_code_address_phase=%zu",
                 (size_t)((uintptr_t)reservation.mapped % plan.alignment_bytes));
        // These are address calculations only; reservation.mapped has not
        // been mapped RX and none of these function pointers are callable.
        if (entry.status == ENGINE_ENTRY_INSPECTED && entry.has_jni_onload &&
            entry.jni_onload_virtual_address >= plan.virtual_page_base) {
            const uint64_t delta = entry.jni_onload_virtual_address -
                                   plan.virtual_page_base;
            if (delta < plan.image_bytes && delta <= UINTPTR_MAX -
                (uintptr_t)reservation.mapped)
                log_line(log, "jni_onload_unmapped_candidate_address=0x%llx",
                         (unsigned long long)((uintptr_t)reservation.mapped + delta));
        }
        if (entry.status == ENGINE_ENTRY_INSPECTED &&
            entry.has_game_activity_initialize_native &&
            entry.game_activity_initialize_native_virtual_address >= plan.virtual_page_base) {
            const uint64_t delta = entry.game_activity_initialize_native_virtual_address -
                                   plan.virtual_page_base;
            if (delta < plan.image_bytes && delta <= UINTPTR_MAX -
                (uintptr_t)reservation.mapped)
                log_line(log, "game_activity_initialize_native_unmapped_candidate_address=0x%llx",
                         (unsigned long long)((uintptr_t)reservation.mapped + delta));
        }
        if (entry.status == ENGINE_ENTRY_INSPECTED &&
            entry.has_app_bridge_start_app &&
            entry.app_bridge_start_app_virtual_address >= plan.virtual_page_base) {
            const uint64_t delta = entry.app_bridge_start_app_virtual_address -
                                   plan.virtual_page_base;
            if (delta < plan.image_bytes && delta <= UINTPTR_MAX -
                (uintptr_t)reservation.mapped)
                log_line(log, "app_bridge_start_app_unmapped_candidate_address=0x%llx",
                         (unsigned long long)((uintptr_t)reservation.mapped + delta));
        }
        EngineStage stage = {0};
        ImportDiagnostics missing = {0};
        log_line(log, "event=strict_final_base_relocation_started");
        const bool ready = engine_stage_stream_resolved_at(
            file, &stage, resolve_compatible, &missing,
            (uint64_t)(uintptr_t)reservation.mapped);
        log_line(log, "strict_stage_status=%d", stage.status);
        char safe[120];
        printable(safe, sizeof(safe), stage.detail);
        log_line(log, "strict_stage_detail=%s", safe);
        log_import_summary(log, "strict", &missing);
        log_line(log, "rela_applied=%u", stage.rela_applied);
        log_line(log, "relr_applied=%u", stage.relr_applied);
        log_line(log, "aps2_applied=%u", stage.packed_applied);
        log_line(log, "symbol_applied=%u", stage.symbol_applied);
        if (ready) {
            result->stage_prepared = true;
            detail(result, "Strict link passed; Android JNI/graphics runtime still missing.");
            log_line(log, "event=strict_stage_prepared");
            if (action != 1)
                log_line(log, "execution_gate=not_requested");
            if (action == 1) {
                bool vm_ready = false, env_ready = false, activity_ready = false;
                void *vm = NULL;
#ifdef MORROW_HAS_JNIVM
                vm = morrow_jni_runtime_java_vm();
                vm_ready = vm != NULL;
                env_ready = morrow_jni_runtime_jni_env() != NULL;
                activity_ready = morrow_jni_runtime_game_activity_ready();
#endif
                // Evaluate mapping eligibility prospectively: no privileged
                // map syscall runs until the constructor/TLS/runtime gates pass.
                const EngineExecutionDecision candidate = engine_execution_decide(
                    true, true, true, &probe, &entry, &plan,
                    (uint64_t)(uintptr_t)reservation.mapped,
                    vm_ready, env_ready, activity_ready);
                log_line(log, "experimental_jni_onload_preflight=%s", candidate.reason);
                if (!candidate.eligible) {
                    log_line(log, "experimental_jni_onload_called=no");
                } else if (stage.image_size != plan.image_bytes ||
                           stage.lowest_virtual_address != plan.virtual_page_base ||
                           stage.intended_runtime_base !=
                               (uint64_t)(uintptr_t)reservation.mapped) {
                    log_line(log, "experimental_jni_onload_blocker=stage_plan_mismatch");
                } else {
                    Result map_error = 0;
                    log_line(log, "event=experimental_engine_rx_mapping_started");
                    if (!horizon_code_memory_map_reserved(&plan, stage.image,
                                                          &reservation, &map_error)) {
                        log_line(log, "experimental_jni_onload_blocker=rx_mapping_failed");
                        log_line(log, "experimental_mapping_kernel_result=0x%08X",
                                 (unsigned)map_error);
                        if (reservation.active) {
                            // A kernel rollback error can leave the image
                            // mapped. Never free its backing source or try
                            // another guest map in the same process.
                            engine_mapping_pinned = true;
                            stage.image = NULL;
                            log_line(log, "blocker=mapped_code_rollback_failed_restart_required");
                        }
                    } else {
                        // Engine-owned JNI registrations can outlive the call.
                        // Retain both mappings even if JNI_OnLoad returns error.
                        engine_entry_started = true;
                        engine_mapping_pinned = true;
                        stage.image = NULL;
                        log_line(log, "engine_mapped_executable=yes");
                        log_line(log, "event=entering_real_engine_jni_onload");
                        detail(result, "Entered Roblox JNI_OnLoad; inspect link report and restart.");
                        const int32_t version = engine_execution_call_jni_onload(
                            candidate.target, vm);
                        log_line(log, "event=real_engine_jni_onload_returned");
                        log_line(log, "engine_jni_onload_return=0x%08X", (unsigned)version);
                        log_line(log, "experimental_jni_onload_called=yes");
                        engine_jni_onload_succeeded = version == 0x00010006;
                        log_line(log, "engine_jni_onload_accepted=%s",
                                 engine_jni_onload_succeeded ? "yes" : "no");
                        if (engine_jni_onload_succeeded &&
                            entry.status == ENGINE_ENTRY_INSPECTED &&
                            entry.has_game_activity_initialize_native &&
                            engine_execution_rx_target(&plan,
                                (uint64_t)(uintptr_t)reservation.mapped,
                                entry.game_activity_initialize_native_virtual_address,
                                &live_game_activity_target)) {
                            log_line(log, "game_activity_mapped_entry_ready=yes");
                        } else {
                            log_line(log, "game_activity_mapped_entry_ready=no");
                        }
                        log_line(log, "engine_gameplay_started=no");
                    }
                }
            }
        } else {
            detail(result, "Real engine entry blocked; see complete link report.");
            // Strict binding stops at the first unknown symbol. Continue a
            // separate, still-NX scan at that *same* final base to reveal the
            // full import gap without manufacturing dummy function pointers.
            EngineStage audit = {0};
            ImportDiagnostics all_imports = {0};
            log_line(log, "event=full_final_base_audit_started");
            const bool all_ready = engine_stage_stream_audit_at(
                file, &audit, resolve_compatible, &all_imports,
                (uint64_t)(uintptr_t)reservation.mapped);
            log_line(log, "full_audit_status=%d", audit.status);
            printable(safe, sizeof(safe), audit.detail);
            log_line(log, "full_audit_detail=%s", safe);
            log_import_summary(log, "full_audit", &all_imports);
            log_line(log, "full_audit_unsupported_relocations=%u",
                     audit.unsupported_relocations_seen);
            log_line(log, "full_audit_tls_relocations=%u", audit.tls_relocations_seen);
            if (all_ready) {
                log_line(log, "execution_gate=blocked_strict_link_inconsistency");
            } else if (all_imports.missing) {
                log_line(log, "execution_gate=blocked_unresolved_android_imports");
            } else {
                log_line(log, "execution_gate=blocked_relocation_or_elf_incompatibility");
            }
            engine_stage_release(&audit);
            log_line(log, "blocker=strict_relocation_incomplete");
        }
        engine_stage_release(&stage);
    }
    if (reservation.reservation && !engine_mapping_pinned) {
        if (!horizon_code_memory_unmap(&reservation, &kernel_error)) {
            detail(result, "Failed to release reserved code address.");
            log_line(log, "blocker=reservation_release_failed");
            result->stage_prepared = false;
        } else {
            log_line(log, "event=reserved_address_released");
        }
    }
    engine_memory_plan_release(&plan);
    fclose(file);

done:
    log_line(log, "engine_entry_called=%s", engine_entry_started ? "yes" : "no");
    log_line(log, "game_activity_initialize_native_called=%s",
             game_activity_entry_called ? "yes" : "no");
    log_line(log, "engine_executed=%s", engine_entry_started ? "yes" : "no");
    log_line(log, "engine_mapped_executable=%s",
             reservation.active ? "yes" : "no");
    const bool all_written = !ferror(log);
    result->log_saved = all_written && fclose(log) == 0;
    if (!all_written) fclose(log);
}
