#ifndef CORDIAL_SWITCH_ENGINE_ENTRY_PROBE_H
#define CORDIAL_SWITCH_ENGINE_ENTRY_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    ENGINE_ENTRY_INVALID = 0,
    ENGINE_ENTRY_IO_ERROR,
    ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS,
    ENGINE_ENTRY_INSPECTED
} EngineEntryProbeStatus;

typedef struct {
    EngineEntryProbeStatus status;
    bool has_jni_onload;
    bool has_native_activity_oncreate;
    bool has_init;
    bool has_init_array;
    uint64_t jni_onload_virtual_address;
    uint64_t native_activity_virtual_address;
    uint64_t init_virtual_address;
    uint64_t init_array_virtual_address;
    unsigned init_array_entries;
    char detail[112];
} EngineEntryProbe;

// Read-only ARM64 ELF entry discovery. Calls no guest code and never guesses
// an exported function from an import or from a truncated section table.
void engine_entry_probe_stream(FILE *file, EngineEntryProbe *result);

#endif
