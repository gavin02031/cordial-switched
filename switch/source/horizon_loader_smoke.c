#include "horizon_loader_smoke.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elf_probe.h"
#include "engine_loader_fixture.h"
#include "engine_runtime_image.h"
#include "horizon_engine_image.h"

// A failed kernel unmap retains its aligned source for an R3 cleanup retry.
static HorizonEngineImage horizon_image;
static EngineRuntimeImage runtime_image;

static FILE *open_numbered(const char *prefix, const char *suffix,
                           char *path, size_t capacity)
{
    for (unsigned n = 1; n <= 99; n++) {
        const int count = snprintf(path, capacity, ENGINE_REPORT_DIR "/%s-%02u.%s",
                                   prefix, n, suffix);
        if (count <= 0 || (size_t)count >= capacity) break;
        const int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        FILE *stream = fdopen(fd, "w+b");
        if (stream) return stream;
        close(fd);
        remove(path);
        break;
    }
    path[0] = 0;
    return NULL;
}
static void trace_line(FILE *file, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fflush(file);
}
static void fail(LoaderSmokeResult *result, const char *message)
{
    result->status = LOADER_SMOKE_FAILED;
    snprintf(result->detail, sizeof(result->detail), "%s", message);
}

void horizon_loader_smoke_run(LoaderSmokeResult *result)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    if (mkdir(ENGINE_REPORT_DIR, 0777) != 0 && errno != EEXIST) {
        fail(result, "Cannot create the SD diagnostic folder.");
        return;
    }
    FILE *trace = open_numbered("map", "txt", result->log_path,
                                sizeof(result->log_path));
    if (!trace) {
        fail(result, "Cannot open an SD-card mapping log.");
        return;
    }
    trace_line(trace, "# Cordial Switched v0.25 synthetic ELF loader check");
    trace_line(trace, "payload=original_two_instruction_arm64_test");
    trace_line(trace, "roblox_library_read=no");
    trace_line(trace, "roblox_engine_executed=no");
    trace_line(trace, "event=test_started");

    const EngineImagePlatform platform = horizon_engine_image_platform(&horizon_image);
    if (runtime_image.reserved) {
        trace_line(trace, "event=retry_previous_cleanup");
        if (!engine_runtime_image_release(&platform, &runtime_image)) {
            result->status = LOADER_SMOKE_CLEANUP_FAILED;
            snprintf(result->detail, sizeof(result->detail),
                     "Previous map cleanup failed (0x%08X).",
                     (unsigned)horizon_image.last_error);
            trace_line(trace, "blocker=previous_mapping_cleanup_failed");
            goto done;
        }
    }

    char path[112];
    FILE *fixture = open_numbered("map-fixture", "tmp", path, sizeof(path));
    if (!fixture) {
        fail(result, "Cannot create synthetic ELF fixture.");
        trace_line(trace, "blocker=fixture_open_failed");
        goto done;
    }
    unsigned char bytes[ENGINE_LOADER_FIXTURE_BYTES];
    engine_loader_fixture_make(bytes);
    const bool wrote = fwrite(bytes, 1, sizeof(bytes), fixture) == sizeof(bytes) &&
                       fflush(fixture) == 0 && fseek(fixture, 0, SEEK_SET) == 0;
    if (!wrote) {
        fail(result, "Cannot save synthetic ELF fixture.");
        trace_line(trace, "blocker=fixture_write_failed");
    } else {
        trace_line(trace, "event=reserve_stage_map_started");
        const bool mapped = engine_runtime_image_prepare(
            fixture, NULL, NULL, &platform, &runtime_image);
        trace_line(trace, "image_status=%d", runtime_image.status);
        trace_line(trace, "stage_status=%d", runtime_image.stage_status);
        trace_line(trace, "kernel_result=0x%08X", (unsigned)horizon_image.last_error);
        trace_line(trace, "image_detail=%s", runtime_image.detail);
        if (!mapped) {
            fail(result, runtime_image.detail);
            trace_line(trace, "blocker=reserve_stage_or_map_failed");
        } else {
            trace_line(trace, "event=code_memory_mapped");
            const uintptr_t base = (uintptr_t)runtime_image.runtime_base;
            uint64_t pointer = 0;
            uint32_t first = 0, second = 0;
            memcpy(&pointer, (const void *)(base + ENGINE_LOADER_POINTER_OFFSET), 8);
            memcpy(&first, (const void *)(base + ENGINE_LOADER_FUNCTION_OFFSET), 4);
            memcpy(&second, (const void *)(base + ENGINE_LOADER_FUNCTION_OFFSET + 4), 4);
            if (pointer != base + ENGINE_LOADER_FUNCTION_OFFSET ||
                first != 0x52800540u || second != 0xD65F03C0u) {
                fail(result, "Mapped bytes or relocation differ.");
                trace_line(trace, "blocker=mapped_bytes_or_relocation_incorrect");
            } else {
                // Flushing this boundary preserves a partial log after a fault.
                trace_line(trace, "event=calling_synthetic_two_instruction_function");
                int (*synthetic)(void) =
                    (int (*)(void))(base + ENGINE_LOADER_FUNCTION_OFFSET);
                const int returned = synthetic();
                trace_line(trace, "synthetic_return=%d", returned);
                if (returned == 42) {
                    result->status = LOADER_SMOKE_VERIFIED;
                    snprintf(result->detail, sizeof(result->detail),
                             "Synthetic ELF mapped and returned 42.");
                    trace_line(trace, "event=synthetic_elf_verified");
                } else {
                    fail(result, "Synthetic ELF returned wrong value.");
                    trace_line(trace, "blocker=synthetic_return_incorrect");
                }
            }
        }
    }
    fclose(fixture);
    if (remove(path) != 0) trace_line(trace, "warning=fixture_not_removed");

    if (runtime_image.reserved &&
        !engine_runtime_image_release(&platform, &runtime_image)) {
        result->status = LOADER_SMOKE_CLEANUP_FAILED;
        snprintf(result->detail, sizeof(result->detail),
                 "Code-memory cleanup failed (0x%08X).",
                 (unsigned)horizon_image.last_error);
        trace_line(trace, "blocker=code_memory_cleanup_failed");
    } else {
        trace_line(trace, "event=code_memory_released");
    }

done:
    trace_line(trace, "smoke_status=%d", result->status);
    trace_line(trace, "roblox_engine_executed=no");
    const bool complete = !ferror(trace);
    result->log_saved = complete && fclose(trace) == 0;
    if (!complete) fclose(trace);
}
