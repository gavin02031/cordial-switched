#include "horizon_code_memory.h"

#include <stdint.h>
#include <string.h>

static bool supported(void)
{
    return envIsSyscallHinted(0x77) && envIsSyscallHinted(0x78) &&
           envIsSyscallHinted(0x73) &&
           envGetOwnProcessHandle() != INVALID_HANDLE;
}

bool horizon_code_memory_map(const EngineMemoryPlan *plan, void *aligned_source,
                             HorizonCodeMemory *mapping, Result *error)
{
    if (error) *error = 0;
    if (!mapping) return false;
    if (mapping->active) return false;
    memset(mapping, 0, sizeof(*mapping));
    if (!engine_memory_plan_validate(plan) || !aligned_source ||
        ((uintptr_t)aligned_source % ENGINE_MEMORY_PAGE_BYTES) != 0 ||
        !supported())
        return false;

    virtmemLock();
    void *target = virtmemFindCodeMemory(plan->image_bytes, ENGINE_MEMORY_PAGE_BYTES);
    void *reservation = target ? virtmemAddReservation(target, plan->image_bytes) : NULL;
    virtmemUnlock();
    if (!reservation)
        return false;

    const Handle process = envGetOwnProcessHandle();
    armDCacheFlush(aligned_source, plan->image_bytes);
    Result rc = svcMapProcessCodeMemory(process, (uint64_t)target,
                                        (uint64_t)aligned_source, plan->image_bytes);
    if (R_FAILED(rc)) {
        virtmemLock();
        virtmemRemoveReservation(reservation);
        virtmemUnlock();
        if (error) *error = rc;
        return false;
    }

    for (size_t i = 0; i < plan->run_count; i++) {
        const EngineMemoryRun *run = &plan->runs[i];
        const u32 permission =
            run->permission == ENGINE_MEMORY_READ_EXECUTE ? Perm_Rx :
            run->permission == ENGINE_MEMORY_READ_WRITE ? Perm_Rw : Perm_R;
        rc = svcSetProcessMemoryPermission(process,
                                           (uint64_t)target + run->offset_bytes,
                                           run->size_bytes, permission);
        if (R_FAILED(rc)) {
            // Always try to restore the same state for every page before
            // unmapping. Do not return a partially mapped module to a caller.
            Result undo = svcSetProcessMemoryPermission(process, (uint64_t)target,
                                                         plan->image_bytes, Perm_Rw);
            if (R_SUCCEEDED(undo))
                undo = svcUnmapProcessCodeMemory(process, (uint64_t)target,
                                                 (uint64_t)aligned_source,
                                                 plan->image_bytes);
            if (R_SUCCEEDED(undo)) {
                virtmemLock();
                virtmemRemoveReservation(reservation);
                virtmemUnlock();
            } else {
                *mapping = (HorizonCodeMemory){aligned_source, target, reservation,
                                               plan->image_bytes, true};
            }
            if (error) *error = R_FAILED(undo) ? undo : rc;
            return false;
        }
    }

    armICacheInvalidate(target, plan->image_bytes);
    *mapping = (HorizonCodeMemory){aligned_source, target, reservation,
                                   plan->image_bytes, true};
    return true;
}

bool horizon_code_memory_unmap(HorizonCodeMemory *mapping, Result *error)
{
    if (error) *error = 0;
    if (!mapping || !mapping->active || !supported())
        return false;
    const Handle process = envGetOwnProcessHandle();
    Result rc = svcSetProcessMemoryPermission(process, (uint64_t)mapping->mapped,
                                              mapping->size_bytes, Perm_Rw);
    if (R_SUCCEEDED(rc))
        rc = svcUnmapProcessCodeMemory(process, (uint64_t)mapping->mapped,
                                       (uint64_t)mapping->source, mapping->size_bytes);
    if (R_FAILED(rc)) {
        if (error) *error = rc;
        return false;
    }
    virtmemLock();
    virtmemRemoveReservation(mapping->reservation);
    virtmemUnlock();
    memset(mapping, 0, sizeof(*mapping));
    return true;
}
