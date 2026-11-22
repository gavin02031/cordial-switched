#include <assert.h>
#include <string.h>

#include "engine_memory_budget.h"

int main(void)
{
    char line[256];
    EngineMemoryBudgetInput in = {1, 1000, 1, 400, 1, 500};
    assert(engine_memory_budget_verdict(&in) == ENGINE_MEMORY_BUDGET_FILE_BELOW_FREE);
    engine_memory_budget_format(&in, line, sizeof line);
    assert(strstr(line, "free=600"));
    assert(strstr(line, "file_below_free_not_a_fit_claim"));

    in.file_bytes = 601; // exactly one over free
    assert(engine_memory_budget_verdict(&in) == ENGINE_MEMORY_BUDGET_FILE_EXCEEDS_FREE);
    in.file_bytes = 600; // equal is not over
    assert(engine_memory_budget_verdict(&in) == ENGINE_MEMORY_BUDGET_FILE_BELOW_FREE);

    // Unavailable numbers are never printed as zero and force unknown.
    EngineMemoryBudgetInput bad = {0, 0, 1, 400, 1, 500};
    assert(engine_memory_budget_verdict(&bad) == ENGINE_MEMORY_BUDGET_UNKNOWN);
    engine_memory_budget_format(&bad, line, sizeof line);
    assert(strstr(line, "total=unavailable") && strstr(line, "free=unavailable"));
    assert(strstr(line, "verdict=unknown"));

    // used > total is inconsistent: unknown, free unavailable.
    EngineMemoryBudgetInput odd = {1, 100, 1, 200, 1, 10};
    assert(engine_memory_budget_verdict(&odd) == ENGINE_MEMORY_BUDGET_UNKNOWN);
    engine_memory_budget_format(&odd, line, sizeof line);
    assert(strstr(line, "free=unavailable"));

    assert(engine_memory_budget_verdict(NULL) == ENGINE_MEMORY_BUDGET_UNKNOWN);
    // Truncation is safe and reports the full length.
    char tiny[8];
    assert(engine_memory_budget_format(&in, tiny, sizeof tiny) > sizeof tiny - 1);
    return 0;
}
