#ifndef MORROW_ENGINE_ALIAS_TRANSACTION_H
#define MORROW_ENGINE_ALIAS_TRANSACTION_H
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t (*EngineAliasOperation)(void *context);
typedef struct {
    EngineAliasOperation map;
    EngineAliasOperation protect;
    EngineAliasOperation unmap;
    void *context;
} EngineAliasOperations;
typedef struct {
    bool ready;
    bool alias_retained;
    uint32_t error;
} EngineAliasResult;

// Callers own the reservation. They may release it only if alias_retained
// is false; a failed rollback must remain pinned for later cleanup.
EngineAliasResult engine_alias_prepare(const EngineAliasOperations *ops);
#endif
