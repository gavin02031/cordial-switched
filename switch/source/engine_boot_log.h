#ifndef CORDIAL_SWITCH_ENGINE_BOOT_LOG_H
#define CORDIAL_SWITCH_ENGINE_BOOT_LOG_H

#include <stdbool.h>
#include <stdio.h>

#include "elf_probe.h"
#include "engine_stage.h"

typedef struct {
    EngineProbeStatus probe_status;
    EngineStageStatus stage_status;
    bool stage_attempted;
    bool engine_executed;
    char detail[120];
} EngineBootReport;

// This is an opt-in preflight against a user-supplied stream, not execution of
// Roblox. The trace is flushed before costly staging so it survives failures.
void engine_boot_preflight(FILE *source, FILE *trace, EngineBootReport *report);

#endif
