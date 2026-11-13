#include "engine_boot_log.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "android_log_compat.h"
#include "android_math_compat.h"
#include "android_libc_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"
#include "android_pthread_compat.h"
#include "android_tls_compat.h"
#include "android_thread_attr_compat.h"
#include "android_thread_lifecycle_compat.h"
#include "engine_tls_scratch.h"

typedef struct {
    unsigned requested;
    unsigned supported;
    char first_missing[120];
    unsigned missing;
    char examples[24][120];
} ImportTrace;

static void trace_line(FILE *trace, const char *format, ...)
{
    if (!trace)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(trace, format, args);
    va_end(args);
    fputc('\n', trace);
    fflush(trace);
}

// Names originate in an untrusted ELF. Keep control bytes and newlines out
// of the report, which users will paste into issue trackers.
static void printable(char *out, size_t size, const char *value)
{
    if (!size)
        return;
    size_t i = 0;
    if (value) {
        for (; value[i] && i + 1 < size; i++) {
            const unsigned char ch = (unsigned char)value[i];
            out[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        }
    }
    out[i] = 0;
}

static bool resolve_compatible_import(const char *name, const char *version,
                                      const char *library, uint64_t *address,
                                      void *context)
{
    ImportTrace *imports = context;
    imports->requested++;
    // Bind only provider/version-verified functions. Bionic FILE data symbols
    // require opaque wrappers; pthread internals must never alias newlib.
    const bool supported = ((!version && !library &&
                             android_log_compat_resolve(name, address, NULL)) ||
                            android_math_compat_resolve(name, version, library, address) ||
                            android_libc_compat_resolve(name, version, library, address) ||
                            android_stdio_compat_resolve(name, version, library, address) ||
                            android_fortify_compat_resolve(name, version, library, address) ||
                            android_pthread_compat_resolve(name, version, library, address) ||
                            android_tls_compat_resolve(name, version, library, address) ||
                            android_thread_attr_compat_resolve(name, version, library, address) ||
                            android_thread_lifecycle_compat_resolve(name, version, library, address));
    if (supported) {
        imports->supported++;
        return true;
    }
    if (!imports->first_missing[0]) {
        char safe_name[64], safe_version[36], safe_library[36];
        printable(safe_name, sizeof(safe_name), name);
        printable(safe_version, sizeof(safe_version), version);
        printable(safe_library, sizeof(safe_library), library);
        snprintf(imports->first_missing, sizeof(imports->first_missing),
                 "%.50s @ %.20s from %.20s", safe_name,
                 version ? safe_version : "unversioned",
                 library ? safe_library : "unspecified");
    }
    if (imports->missing < 24) {
        char safe_name[64], safe_version[36], safe_library[36];
        printable(safe_name, sizeof(safe_name), name);
        printable(safe_version, sizeof(safe_version), version);
        printable(safe_library, sizeof(safe_library), library);
        snprintf(imports->examples[imports->missing],
                 sizeof(imports->examples[0]), "%.50s @ %.20s from %.20s",
                 safe_name, version ? safe_version : "unversioned",
                 library ? safe_library : "unspecified");
    }
    if (imports->missing != UINT32_MAX)
        imports->missing++;
    return false;
}

static const char *probe_status_name(EngineProbeStatus status)
{
    switch (status) {
    case ENGINE_PROBE_INSPECTED: return "INSPECTED";
    case ENGINE_PROBE_NOT_FOUND: return "NOT_FOUND";
    case ENGINE_PROBE_WRONG_ARCH: return "WRONG_ARCH";
    case ENGINE_PROBE_IO_ERROR: return "IO_ERROR";
    default: return "INVALID_ELF";
    }
}

static const char *stage_status_name(EngineStageStatus status)
{
    switch (status) {
    case ENGINE_STAGE_PREPARED: return "SCRATCH_STAGED";
    case ENGINE_STAGE_UNRESOLVED_SYMBOL: return "UNRESOLVED_SYMBOL";
    case ENGINE_STAGE_UNSUPPORTED_RELOCATION: return "UNSUPPORTED_RELOCATION";
    case ENGINE_STAGE_TOO_LARGE: return "TOO_LARGE";
    case ENGINE_STAGE_IO_ERROR: return "IO_ERROR";
    default: return "INVALID_ELF";
    }
}

static const char *tls_status_name(EngineTlsScratchStatus status)
{
    switch (status) {
    case ENGINE_TLS_ABSENT: return "ABSENT";
    case ENGINE_TLS_PREPARED: return "SCRATCH_COPIED";
    case ENGINE_TLS_TOO_LARGE: return "TOO_LARGE";
    case ENGINE_TLS_IO_ERROR: return "IO_ERROR";
    case ENGINE_TLS_NO_MEMORY: return "NO_MEMORY";
    default: return "INVALID_TEMPLATE";
    }
}

void engine_boot_preflight(FILE *source, FILE *trace, EngineBootReport *report)
{
    if (!report)
        return;
    memset(report, 0, sizeof(*report));
    trace_line(trace, "# Cordial Switched ARM64 engine bootstrap audit v0.23");
    trace_line(trace, "# Engine instructions are NEVER executed by this diagnostic.");
    trace_line(trace, "event=preflight_started");

    if (!source) {
        report->probe_status = ENGINE_PROBE_NOT_FOUND;
        snprintf(report->detail, sizeof(report->detail), "Engine file not found on SD card.");
        trace_line(trace, "inspect=NOT_FOUND");
        trace_line(trace, "blocker=ENGINE_FILE_MISSING");
        trace_line(trace, "engine_executed=no");
        return;
    }

    EngineProbeResult profile;
    engine_probe_stream(source, &profile);
    report->probe_status = profile.status;
    char safe[120];
    printable(safe, sizeof(safe), profile.detail);
    trace_line(trace, "inspect=%s", probe_status_name(profile.status));
    trace_line(trace, "inspect_detail=%s", safe);
    if (profile.status != ENGINE_PROBE_INSPECTED) {
        snprintf(report->detail, sizeof(report->detail), "%s", profile.detail);
        trace_line(trace, "blocker=INSPECTION_FAILED");
        trace_line(trace, "engine_executed=no");
        return;
    }

    trace_line(trace, "file_bytes=%llu", (unsigned long long)profile.file_bytes);
    trace_line(trace, "virtual_image_bytes=%llu",
               (unsigned long long)profile.virtual_image_bytes);
    trace_line(trace, "load_segments=%u", profile.load_segments);
    trace_line(trace, "android_dependencies=%u", profile.needed_count);
    trace_line(trace, "tls_segment=%s", profile.has_tls_segment ? "present" : "absent");
    if (profile.has_tls_segment) {
        trace_line(trace, "tls_initial_image_bytes=%llu",
                   (unsigned long long)profile.tls_initial_image_bytes);
        trace_line(trace, "tls_memory_bytes=%llu",
                   (unsigned long long)profile.tls_memory_bytes);
        trace_line(trace, "tls_alignment_bytes=%llu",
                   (unsigned long long)profile.tls_alignment);
    }
    trace_line(trace, "tls_thread_pointer_installed=no");
    EngineTlsScratch tls_scratch = {0};
    EngineTlsScratchStatus tls_status = engine_tls_scratch_prepare(source, &profile,
                                                                   &tls_scratch);
    trace_line(trace, "tls_scratch_copy=%s", tls_status_name(tls_status));
    trace_line(trace, "tls_scratch_active=no");
    engine_tls_scratch_release(&tls_scratch);
    for (unsigned i = 0; i < profile.needed_count && i < ENGINE_PROBE_MAX_NEEDED; i++) {
        char library[64];
        printable(library, sizeof(library), profile.needed[i]);
        trace_line(trace, "required_library_%u=%s", i + 1, library);
    }
    trace_line(trace, "aps2_bytes=%llu", (unsigned long long)profile.android_packed_bytes);
    trace_line(trace, "plt_relocations=%llu", (unsigned long long)profile.plt_records);
    trace_line(trace, "versioned_plt_imports=%llu",
               (unsigned long long)profile.plt_versioned_imports);
    trace_line(trace, "apk_signature_verified=no");
    trace_line(trace, "platform_bionic_jni_gameactivity=not_connected");
    trace_line(trace, "platform_egl_gles=not_connected");
    trace_line(trace, "provider_bionic_stdio=partial_wrapped");
    trace_line(trace, "provider_bionic_libc=partial_abi_checked");
    trace_line(trace, "provider_bionic_fortify=checked_bounds_partial");
    trace_line(trace, "provider_bionic_pthread=mutex_cond_once_partial");
    trace_line(trace, "provider_bionic_tls=pthread_keys_multiplexed_partial");
    trace_line(trace, "provider_bionic_thread_attr=lp64_stack_detach_guard_partial");
    trace_line(trace, "provider_bionic_thread_lifecycle=opaque_native_threads_no_bionic_tcb");
    trace_line(trace, "provider_bionic_arm64_tcb_shadow=prepared_not_installed");
    trace_line(trace, "event=scratch_link_started");
    trace_line(trace, "audit_mode=continue_on_unresolved_imports_without_binding");

    // The real file is read again, but never mapped as executable memory.
    EngineStage stage = {0};
    ImportTrace imports = {0};
    report->stage_attempted = true;
    engine_stage_stream_audit(source, &stage, resolve_compatible_import, &imports);
    report->stage_status = stage.status;
    snprintf(report->detail, sizeof(report->detail), "%s", stage.detail);
    printable(safe, sizeof(safe), stage.detail);
    trace_line(trace, "scratch_link=%s", stage_status_name(stage.status));
    trace_line(trace, "scratch_link_detail=%s", safe);
    trace_line(trace, "import_lookups=%u", imports.requested);
    trace_line(trace, "supported_imports=%u", imports.supported);
    trace_line(trace, "missing_import_lookups=%u", imports.missing);
    if (imports.first_missing[0])
        trace_line(trace, "first_missing_import=%s", imports.first_missing);
    for (unsigned i = 0; i < imports.missing && i < 24; i++)
        trace_line(trace, "missing_import_%u=%s", i + 1, imports.examples[i]);
    if (stage.status == ENGINE_STAGE_PREPARED) {
        trace_line(trace, "relative_relocations=%u", stage.rela_applied);
        trace_line(trace, "relr_relocations=%u", stage.relr_applied);
        trace_line(trace, "aps2_relocations=%u", stage.packed_applied);
        trace_line(trace, "symbol_relocations=%u", stage.symbol_applied);
    }
    // Do not turn a successful scratch link into an engine boot. Missing
    // Android runtime services make calling into this image unsafe.
    trace_line(trace, "blocker=%s", stage.status == ENGINE_STAGE_PREPARED
               ? "ANDROID_RUNTIME_AND_SIGNATURE_VERIFICATION_MISSING"
               : "SCRATCH_LINK_INCOMPLETE");
    trace_line(trace, "engine_executed=no");
    engine_stage_release(&stage);
}
