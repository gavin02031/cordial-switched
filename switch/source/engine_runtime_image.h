#ifndef CORDIAL_ENGINE_RUNTIME_IMAGE_H
#define CORDIAL_ENGINE_RUNTIME_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "engine_memory_plan.h"
#include "engine_stage.h"

// A platform mapping has a separate source and final address. The backend
// owns its reservation until release succeeds, including after a failed map.
// It must never call into the image: Android dependencies and TLS are absent.
typedef struct {
    void *context;
    bool (*reserve)(void *context, const EngineMemoryPlan *plan,
                    uint64_t *runtime_base);
    bool (*map)(void *context, const EngineMemoryPlan *plan,
                void *aligned_source);
    bool (*release)(void *context);
} EngineImagePlatform;

typedef enum {
    ENGINE_IMAGE_NOT_STARTED = 0,
    ENGINE_IMAGE_INVALID_ELF,
    ENGINE_IMAGE_PLAN_FAILED,
    ENGINE_IMAGE_RESERVE_FAILED,
    ENGINE_IMAGE_STAGE_FAILED,
    ENGINE_IMAGE_ALLOC_FAILED,
    ENGINE_IMAGE_MAP_FAILED,
    ENGINE_IMAGE_MAPPED_NOT_EXECUTED,
    ENGINE_IMAGE_CLEANUP_FAILED
} EngineImageStatus;

typedef struct {
    EngineMemoryPlan plan;
    void *aligned_source;
    uint64_t runtime_base;
    bool reserved;
    bool mapped;
    EngineImageStatus status;
    EngineStageStatus stage_status;
    char detail[120];
} EngineRuntimeImage;

// The caller supplies a zero-initialised image. On failure, resources are
// released unless the platform release itself fails; in that case image
// retains ownership and engine_runtime_image_release can be retried.
// A successful return means bytes were MAPPED, never that code was called.
bool engine_runtime_image_prepare(FILE *file, EngineSymbolResolver resolver,
                                  void *resolver_context,
                                  const EngineImagePlatform *platform,
                                  EngineRuntimeImage *image);
bool engine_runtime_image_release(const EngineImagePlatform *platform,
                                  EngineRuntimeImage *image);

#endif
