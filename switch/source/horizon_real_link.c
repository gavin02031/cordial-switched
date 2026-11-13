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
#include "engine_memory_plan.h"
#include "engine_stage.h"
#include "engine_tls_scratch.h"
#include "engine_tls_thread.h"
#include "horizon_code_address.h"
#include "horizon_code_memory.h"

static HorizonCodeMemory reservation;

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

void horizon_real_link_run(RealLinkResult *result)
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
    log_line(log, "# Cordial Switched v0.33 real-engine entry readiness attempt");
    log_line(log, "# No engine entry call without complete relocations and Android runtime.");
    log_line(log, "engine_executed=no");
    log_line(log, "engine_mapped_executable=no");
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
            log_line(log, "event=nx_stage_prepared_not_executed");
            log_line(log, "execution_gate=blocked_android_jni_graphics_runtime");
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
    if (reservation.reservation) {
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
    log_line(log, "engine_entry_called=no");
    log_line(log, "engine_executed=no");
    log_line(log, "engine_mapped_executable=no");
    const bool all_written = !ferror(log);
    result->log_saved = all_written && fclose(log) == 0;
    if (!all_written) fclose(log);
}
