#include "engine_alias_transaction.h"

EngineAliasResult engine_alias_prepare(const EngineAliasOperations *ops)
{
    uint32_t error = ops->map(ops->context);
    if (error) return (EngineAliasResult){false, false, error};
    error = ops->protect(ops->context);
    if (!error) return (EngineAliasResult){true, true, 0};
    uint32_t rollback = ops->unmap(ops->context);
    return (EngineAliasResult){false, rollback != 0, rollback ? rollback : error};
}
