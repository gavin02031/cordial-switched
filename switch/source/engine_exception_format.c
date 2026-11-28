#include "engine_exception_format.h"

#include <stdio.h>

int engine_exception_format(char *buffer, size_t capacity, uint32_t description,
                            uint64_t pc, uint64_t x8, uint32_t esr,
                            uint64_t far, const char *build_id,
                            uint64_t system_tick, uint64_t handler_pc)
{
    if (!buffer || !capacity) return 0;
    const int count = snprintf(
        buffer, capacity,
        "morrow_exception kind=0x%08x pc=0x%016llx x8=0x%016llx "
        "esr=0x%08x far=0x%016llx\n"
        "morrow_exception_provenance build=%.64s tick=%llu handler_pc=0x%016llx\n",
        (unsigned)description, (unsigned long long)pc,
        (unsigned long long)x8, (unsigned)esr, (unsigned long long)far,
        build_id ? build_id : "unknown", (unsigned long long)system_tick,
        (unsigned long long)handler_pc);
    return count >= 0 && (size_t)count < capacity;
}
