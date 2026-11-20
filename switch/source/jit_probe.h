#ifndef CORDIAL_SWITCH_JIT_PROBE_H
#define CORDIAL_SWITCH_JIT_PROBE_H

#include <stdint.h>

// Only the known two-instruction test payload can execute. Never hand this
// probe an ELF image, Roblox library, file path, or arbitrary code bytes.
typedef enum {
    JIT_PROBE_NOT_RUN = 0,
    JIT_PROBE_UNAVAILABLE,
    JIT_PROBE_BAD_MAPPING,
    JIT_PROBE_TRANSITION_FAILED,
    JIT_PROBE_WRONG_RESULT,
    JIT_PROBE_CLEANUP_FAILED,
    JIT_PROBE_VERIFIED
} JitProbeStatus;

typedef struct {
    JitProbeStatus status;
    uint32_t error_code;
    char detail[100];
} JitProbeResult;

void jit_probe_run(JitProbeResult *result);

#endif
