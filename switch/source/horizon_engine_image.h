#ifndef CORDIAL_HORIZON_ENGINE_IMAGE_H
#define CORDIAL_HORIZON_ENGINE_IMAGE_H

#include "engine_runtime_image.h"
#include "horizon_code_memory.h"

typedef struct {
    HorizonCodeMemory mapping;
    Result last_error;
} HorizonEngineImage;

// Connect the portable reserve/relocate/map sequence to Horizon. This only
// constructs a code-memory mapping; there is deliberately no entry-point API.
EngineImagePlatform horizon_engine_image_platform(HorizonEngineImage *context);

#endif
