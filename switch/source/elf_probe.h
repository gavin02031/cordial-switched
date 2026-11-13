#ifndef CORDIAL_SWITCH_ELF_PROBE_H
#define CORDIAL_SWITCH_ELF_PROBE_H

#include <stdint.h>
#include <stdio.h>

// A user-supplied Android ARM64 library. Never commit the file to this repo.
#define ENGINE_LIBRARY_PATH "sdmc:/switch/cordial-switched/libroblox.so"
#define ENGINE_PROBE_MAX_NEEDED 5

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
    unsigned load_segments;
    unsigned needed_count;
    char needed[ENGINE_PROBE_MAX_NEEDED][64];
    char detail[112];
} EngineProbeResult;

// Inspection is read-only. No executable pages are mapped, relocated or called.
void engine_probe_stream(FILE *file, EngineProbeResult *result);
void engine_probe_file(const char *path, EngineProbeResult *result);

#endif
