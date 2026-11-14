#include "jit_probe.h"

#include <stdio.h>
#include <string.h>

#include <switch.h>

enum { JIT_PAGE_BYTES = 0x1000, EXPECTED_RETURN = 42 };

static void status(JitProbeResult *result, JitProbeStatus state,
                   Result error, const char *message)
{
    result->status = state;
    result->error_code = error;
    if (R_FAILED(error))
        snprintf(result->detail, sizeof(result->detail), "%s (0x%08X)", message, error);
    else
        snprintf(result->detail, sizeof(result->detail), "%s", message);
}

void jit_probe_run(JitProbeResult *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof(*result));

    // AArch64 little-endian: movz w0, #42 ; ret.
    // These eight hard-coded bytes are our entire payload; no file is loaded.
    static const uint32_t instructions[] = {0x52800540u, 0xD65F03C0u};
    Jit jit = {0};
    Result rc = jitCreate(&jit, JIT_PAGE_BYTES);
    if (R_FAILED(rc)) {
        status(result, JIT_PROBE_UNAVAILABLE, rc, "Homebrew JIT memory unavailable");
        return;
    }

    void *rw = jitGetRwAddr(&jit);
    void *rx = jitGetRxAddr(&jit);
    if (!rw || !rx) {
        status(result, JIT_PROBE_BAD_MAPPING, 0, "JIT aliases were not mapped");
    } else {
        memcpy(rw, instructions, sizeof(instructions));
        armDCacheFlush(rw, sizeof(instructions));

        rc = jitTransitionToExecutable(&jit);
        if (R_FAILED(rc)) {
            status(result, JIT_PROBE_TRANSITION_FAILED, rc, "Could not switch JIT memory to RX");
        } else {
            armICacheInvalidate(rx, sizeof(instructions));
            // libnx has established RX permissions. This calls only our two
            // instructions, not the engine or user-supplied input.
            int (*test_function)(void) = (int (*)(void))rx;
            const int actual = test_function();
            if (actual == EXPECTED_RETURN)
                status(result, JIT_PROBE_VERIFIED, 0, "ARM64 test returned 42");
            else
                status(result, JIT_PROBE_WRONG_RESULT, 0, "ARM64 test returned a wrong value");
        }
    }

    rc = jitClose(&jit);
    if (R_FAILED(rc))
        status(result, JIT_PROBE_CLEANUP_FAILED, rc, "Could not release JIT buffer");
}
