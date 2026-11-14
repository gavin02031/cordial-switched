#include "engine_runtime_image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool engine_runtime_image_release(const EngineImagePlatform *platform,
                                  EngineRuntimeImage *image)
{
    if (!image)
        return false;
    if (image->reserved) {
        if (!platform || !platform->release || !platform->release(platform->context)) {
            image->status = ENGINE_IMAGE_CLEANUP_FAILED;
            snprintf(image->detail, sizeof(image->detail),
                     "Could not release reserved/mapped code memory.");
            return false;
        }
    }
    image->reserved = false;
    image->mapped = false;
    image->runtime_base = 0;
    free(image->aligned_source);
    image->aligned_source = NULL;
    engine_memory_plan_release(&image->plan);
    return true;
}

static bool failed(const EngineImagePlatform *platform, EngineRuntimeImage *image)
{
    engine_runtime_image_release(platform, image);
    return false;
}

bool engine_runtime_image_prepare(FILE *file, EngineSymbolResolver resolver,
                                  void *resolver_context,
                                  const EngineImagePlatform *platform,
                                  EngineRuntimeImage *image)
{
    if (!image || image->reserved || image->aligned_source || image->plan.runs)
        return false;
    memset(image, 0, sizeof(*image));
    if (!file || !platform || !platform->reserve || !platform->map ||
        !platform->release) {
        image->status = ENGINE_IMAGE_INVALID_ELF;
        snprintf(image->detail, sizeof(image->detail), "Missing ELF or mapping backend.");
        return false;
    }

    EngineProbeResult probe;
    engine_probe_stream(file, &probe);
    if (probe.status != ENGINE_PROBE_INSPECTED) {
        image->status = ENGINE_IMAGE_INVALID_ELF;
        snprintf(image->detail, sizeof(image->detail), "%.119s", probe.detail);
        return false;
    }
    if (!engine_memory_plan_from_probe(&probe, &image->plan)) {
        image->status = ENGINE_IMAGE_PLAN_FAILED;
        snprintf(image->detail, sizeof(image->detail), "ELF cannot use safe RX/R/RW pages.");
        return false;
    }

    uint64_t final_base = 0;
    if (!platform->reserve(platform->context, &image->plan, &final_base)) {
        image->status = ENGINE_IMAGE_RESERVE_FAILED;
        snprintf(image->detail, sizeof(image->detail), "Could not reserve Horizon code address.");
        return failed(platform, image);
    }
    // A successful reservation transfers ownership immediately, even when
    // its chosen address is unusable for this ELF's load bias.
    image->reserved = true;
    image->runtime_base = final_base;
    if (!final_base || (final_base % ENGINE_MEMORY_PAGE_BYTES) ||
        final_base % image->plan.alignment_bytes !=
            image->plan.virtual_page_base % image->plan.alignment_bytes ||
        final_base < image->plan.virtual_page_base ||
        final_base > UINT64_MAX - image->plan.image_bytes) {
        image->status = ENGINE_IMAGE_RESERVE_FAILED;
        snprintf(image->detail, sizeof(image->detail), "Invalid reserved code address.");
        return failed(platform, image);
    }

    EngineStage stage = {0};
    const bool staged = engine_stage_stream_resolved_at(
        file, &stage, resolver, resolver_context, final_base);
    image->stage_status = stage.status;
    if (!staged) {
        image->status = ENGINE_IMAGE_STAGE_FAILED;
        snprintf(image->detail, sizeof(image->detail), "%.119s", stage.detail);
        engine_stage_release(&stage);
        return failed(platform, image);
    }
    // A disagreement between the inspector and linker must never allow the
    // prepared buffer to be copied beyond its independently checked span.
    if (stage.image_size != image->plan.image_bytes ||
        stage.lowest_virtual_address != image->plan.virtual_page_base ||
        stage.intended_runtime_base != final_base) {
        image->status = ENGINE_IMAGE_STAGE_FAILED;
        snprintf(image->detail, sizeof(image->detail), "ELF stage/plan address mismatch.");
        engine_stage_release(&stage);
        return failed(platform, image);
    }

    if (((uintptr_t)stage.image % ENGINE_MEMORY_PAGE_BYTES) != 0) {
        image->status = ENGINE_IMAGE_ALLOC_FAILED;
        snprintf(image->detail, sizeof(image->detail), "Staging source is not page-aligned.");
        engine_stage_release(&stage);
        return failed(platform, image);
    }
    // Transfer the NX buffer directly to Horizon: no 119 MiB duplicate heap
    // allocation while preparing a real-world Android ELF image.
    image->aligned_source = stage.image;
    stage.image = NULL;
    engine_stage_release(&stage);
    if (!platform->map(platform->context, &image->plan, image->aligned_source)) {
        image->status = ENGINE_IMAGE_MAP_FAILED;
        snprintf(image->detail, sizeof(image->detail), "Code-memory mapping failed.");
        return failed(platform, image);
    }
    image->mapped = true;
    image->status = ENGINE_IMAGE_MAPPED_NOT_EXECUTED;
    snprintf(image->detail, sizeof(image->detail),
             "ELF mapped at final base; no Android code executed.");
    return true;
}
