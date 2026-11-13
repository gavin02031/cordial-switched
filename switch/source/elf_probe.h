#ifndef CORDIAL_SWITCH_ELF_PROBE_H
#define CORDIAL_SWITCH_ELF_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// A user-supplied Android ARM64 library. Never commit the file to this repo.
#define ENGINE_LIBRARY_PATH "sdmc:/switch/cordial-switched/libroblox.so"
#define ENGINE_PROBE_MAX_NEEDED 12
#define ENGINE_PROBE_MAX_VISIBLE 5
#define ENGINE_PROBE_MAX_IMPORT_NAMES 12
#define ENGINE_REPORT_DIR "sdmc:/switch/cordial-switched"

typedef enum {
    ENGINE_PROBE_NOT_FOUND = 0,
    ENGINE_PROBE_IO_ERROR,
    ENGINE_PROBE_INVALID_ELF,
    ENGINE_PROBE_WRONG_ARCH,
    ENGINE_PROBE_INSPECTED
} EngineProbeStatus;

typedef struct {
    EngineProbeStatus status;
    uint64_t file_bytes;
    uint64_t max_segment_alignment;
    uint64_t virtual_image_bytes;
    uint64_t rela_records;
    uint64_t plt_records;
    uint64_t plt_imports;
    uint64_t plt_versioned_imports;
    uint64_t plt_log_compat_imports;
    uint64_t relr_words;
    uint64_t android_packed_bytes;
    uint64_t tls_initial_image_bytes;
    uint64_t tls_memory_bytes;
    uint64_t tls_alignment;
    uint64_t tls_file_offset;
    unsigned load_segments;
    unsigned executable_segments;
    unsigned writable_segments;
    unsigned needed_count;
    unsigned captured_import_names;
    // Sizes/counts come from dynamic ELF tags, not from executing a loader.
    bool has_symbol_versions;
    bool has_unsupported_plt_format;
    bool has_tls_segment;
    char needed[ENGINE_PROBE_MAX_NEEDED][64];
    char import_names[ENGINE_PROBE_MAX_IMPORT_NAMES][64];
    char detail[112];
} EngineProbeResult;

// Inspection is read-only. No executable pages are mapped, relocated or called.
void engine_probe_stream(FILE *file, EngineProbeResult *result);
// Explicit opt-in stream output for testing and the on-device report export.
void engine_probe_stream_manifest(FILE *file, EngineProbeResult *result,
                                  FILE *manifest);
void engine_probe_file(const char *path, EngineProbeResult *result);
// Export only printable dynamic-symbol metadata; never include library bytes
// or account data. Uses a new numbered file rather than overwriting old logs.
bool engine_probe_export_imports(const char *path, char *saved_path,
                                  size_t saved_path_bytes);

#endif
