#ifndef CORDIAL_ENGINE_MEMORY_PLAN_H
#define CORDIAL_ENGINE_MEMORY_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Native-loader memory layout (not a Roblox/Android ABI implementation).
// Page permissions are planned before mapping so no executable page ever
// passes through a writable code-memory permission transition.
#define ENGINE_MEMORY_PAGE_BYTES 4096u
#define ENGINE_MEMORY_MAX_BYTES (192u * 1024u * 1024u)

typedef struct {
    uint64_t virtual_address;
    uint64_t memory_bytes;
    bool executable;
    bool writable;
} EngineMemorySegment;

typedef enum {
    ENGINE_MEMORY_READ,
    ENGINE_MEMORY_READ_EXECUTE,
    ENGINE_MEMORY_READ_WRITE
} EngineMemoryPermission;

typedef struct {
    size_t offset_bytes;
    size_t size_bytes;
    EngineMemoryPermission permission;
} EngineMemoryRun;

typedef struct {
    uint64_t virtual_page_base;
    size_t image_bytes;
    size_t executable_bytes;
    size_t writable_bytes;
    size_t readonly_bytes;
    EngineMemoryRun *runs;
    size_t run_count;
} EngineMemoryPlan;

// Reject W+X segments, W/X conflicts within one page, address overflows,
// and images beyond the defined budget. Runs are ordered RX, R, RW; this
// ordering is required by the Switch code-memory permission mechanism.
bool engine_memory_plan_build(const EngineMemorySegment *segments, size_t count,
                              EngineMemoryPlan *plan);
// Recheck run coverage, ordering, alignment and permission values before
// passing the plan to privileged code-memory syscalls.
bool engine_memory_plan_validate(const EngineMemoryPlan *plan);
void engine_memory_plan_release(EngineMemoryPlan *plan);

#endif
