#include "horizon_engine_image.h"

#include <stdint.h>

static bool reserve_image(void *context, const EngineMemoryPlan *plan,
                          uint64_t *runtime_base)
{
    HorizonEngineImage *image = context;
    image->last_error = 0;
    if (!horizon_code_memory_reserve(plan, &image->mapping))
        return false;
    *runtime_base = (uint64_t)(uintptr_t)image->mapping.mapped;
    return true;
}

static bool map_image(void *context, const EngineMemoryPlan *plan, void *source)
{
    HorizonEngineImage *image = context;
    return horizon_code_memory_map_reserved(plan, source, &image->mapping,
                                             &image->last_error);
}

static bool release_image(void *context)
{
    HorizonEngineImage *image = context;
    return horizon_code_memory_unmap(&image->mapping, &image->last_error);
}

EngineImagePlatform horizon_engine_image_platform(HorizonEngineImage *context)
{
    return (EngineImagePlatform){context, reserve_image, map_image, release_image};
}
