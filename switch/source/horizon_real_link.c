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
#include "horizon_code_memory.h"

static HorizonCodeMemory reservation;

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
    (void)context;
    // Match the same real providers used by ZR's import audit, without
    // fabricating pointers for Android services that do not exist on Horizon.
    return ((!version && !library &&
             android_log_compat_resolve(name, address, NULL)) ||
            android_math_compat_resolve(name, version, library, address) ||
            android_libc_compat_resolve(name, version, library, address) ||
            android_stdio_compat_resolve(name, version, library, address) ||
            android_fortify_compat_resolve(name, version, library, address) ||
            android_pthread_compat_resolve(name, version, library, address) ||
            android_tls_compat_resolve(name, version, library, address) ||
            android_thread_attr_compat_resolve(name, version, library, address) ||
            android_thread_lifecycle_compat_resolve(name, version, library, address));
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
    log_line(log, "# Cordial Switched v0.26 final-base real ELF preflight");
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

    EngineMemoryPlan plan = {0};
    if (!engine_memory_plan_from_probe(&probe, &plan)) {
        detail(result, "Cannot construct safe Horizon RX/R/RW plan.");
        log_line(log, "blocker=memory_plan_invalid");
        fclose(file);
        goto done;
    }
    log_line(log, "virtual_image_bytes=%zu", plan.image_bytes);
    log_line(log, "event=reserve_final_address");
    if (!horizon_code_memory_reserve(&plan, &reservation)) {
        detail(result, "Horizon could not reserve a final code address.");
        log_line(log, "blocker=final_address_reservation_failed");
    } else {
        log_line(log, "event=final_address_reserved");
        EngineStage stage = {0};
        log_line(log, "event=strict_final_base_relocation_started");
        const bool ready = engine_stage_stream_resolved_at(
            file, &stage, resolve_compatible, NULL,
            (uint64_t)(uintptr_t)reservation.mapped);
        log_line(log, "strict_stage_status=%d", stage.status);
        char safe[120];
        printable(safe, sizeof(safe), stage.detail);
        log_line(log, "strict_stage_detail=%s", safe);
        log_line(log, "rela_applied=%u", stage.rela_applied);
        log_line(log, "relr_applied=%u", stage.relr_applied);
        log_line(log, "aps2_applied=%u", stage.packed_applied);
        log_line(log, "symbol_applied=%u", stage.symbol_applied);
        if (ready) {
            result->stage_prepared = true;
            detail(result, "Final-address ELF staged; Android runtime still missing.");
            log_line(log, "event=nx_stage_prepared_not_executed");
        } else {
            detail(result, safe);
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
    log_line(log, "engine_executed=no");
    log_line(log, "engine_mapped_executable=no");
    const bool all_written = !ferror(log);
    result->log_saved = all_written && fclose(log) == 0;
    if (!all_written) fclose(log);
}
