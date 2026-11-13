#include "engine_stage.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "elf_probe.h"

enum {
    ELF_HEADER_SIZE = 64,
    PHDR_SIZE = 56,
    MAX_LOAD_SEGMENTS = 128,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    DT_NULL = 0,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_REL = 17,
    DT_RELSZ = 18,
    DT_RELRSZ = 35,
    DT_RELR = 36,
    DT_RELRENT = 37,
    DT_PLTRELSZ = 2,
    DT_JMPREL = 23,
    DT_ANDROID_RELA = 0x60000011,
    DT_ANDROID_RELASZ = 0x60000012,
    DT_ANDROID_REL = 0x6000000f,
    DT_ANDROID_RELSZ = 0x60000010,
    DT_ANDROID_RELR = 0x6fffe000,
    DT_ANDROID_RELRSZ = 0x6fffe001,
    DT_ANDROID_RELRENT = 0x6fffe003,
    R_AARCH64_RELATIVE = 1027,
    PAGE_SIZE = 4096,
};

typedef struct {
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t file_bytes;
    uint64_t memory_bytes;
} Segment;

static uint32_t u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64le(const unsigned char *p)
{
    return (uint64_t)u32le(p) | (uint64_t)u32le(p + 4) << 32;
}

static uint16_t u16le(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static void write64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (unsigned char)(value >> (i * 8));
}

static bool read_at(FILE *file, uint64_t offset, void *buffer, size_t size)
{
    return offset <= LONG_MAX && fseek(file, (long)offset, SEEK_SET) == 0 &&
           fread(buffer, 1, size, file) == size;
}

static bool within(uint64_t offset, uint64_t count, uint64_t limit)
{
    return offset <= limit && count <= limit - offset;
}

static bool align_up(uint64_t value, uint64_t alignment, uint64_t *out)
{
    if (value > UINT64_MAX - (alignment - 1))
        return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static unsigned char *mapped_at(const EngineStage *stage, uint64_t virtual_address,
                                size_t size)
{
    if (virtual_address < stage->lowest_virtual_address)
        return NULL;
    const uint64_t offset = virtual_address - stage->lowest_virtual_address;
    if (!within(offset, size, stage->image_size))
        return NULL;
    return stage->image + (size_t)offset;
}

// Addresses in the gaps between PT_LOAD segments are not valid relocation
// destinations even though the scratch image spans those gaps.
static unsigned char *load_at(const EngineStage *stage, const Segment *segments,
                              unsigned count, uint64_t address, size_t size,
                              bool file_backed)
{
    for (unsigned i = 0; i < count; i++) {
        const Segment *segment = &segments[i];
        if (address < segment->virtual_address)
            continue;
        const uint64_t offset = address - segment->virtual_address;
        if (within(offset, size, file_backed ? segment->file_bytes : segment->memory_bytes))
            return mapped_at(stage, address, size);
    }
    return NULL;
}

static bool abort_stage(EngineStage *stage, EngineStageStatus status, const char *message)
{
    free(stage->image);
    stage->image = NULL;
    stage->image_size = 0;
    stage->rela_applied = 0;
    stage->relr_applied = 0;
    stage->status = status;
    snprintf(stage->detail, sizeof(stage->detail), "%s", message);
    return false;
}

void engine_stage_release(EngineStage *stage)
{
    if (!stage)
        return;
    free(stage->image);
    memset(stage, 0, sizeof(*stage));
}

bool engine_stage_stream(FILE *file, EngineStage *stage)
{
    if (!stage)
        return false;
    memset(stage, 0, sizeof(*stage));

    EngineProbeResult probe;
    engine_probe_stream(file, &probe);
    if (probe.status != ENGINE_PROBE_INSPECTED)
        return abort_stage(stage, ENGINE_STAGE_INVALID, probe.detail);

    unsigned char header[ELF_HEADER_SIZE];
    if (!read_at(file, 0, header, sizeof(header)))
        return abort_stage(stage, ENGINE_STAGE_IO_ERROR, "Could not read the ELF header.");

    const uint64_t ph_offset = u64le(header + 32);
    const unsigned ph_count = u16le(header + 56);
    Segment segments[MAX_LOAD_SEGMENTS];
    unsigned segment_count = 0;
    uint64_t low = UINT64_MAX;
    uint64_t high = 0;
    uint64_t dynamic_vaddr = 0;
    uint64_t dynamic_size = 0;
    for (unsigned i = 0; i < ph_count; i++) {
        unsigned char ph[PHDR_SIZE];
        if (!read_at(file, ph_offset + (uint64_t)i * PHDR_SIZE, ph, sizeof(ph)))
            return abort_stage(stage, ENGINE_STAGE_IO_ERROR, "Could not read PT_LOAD metadata.");
        const uint32_t type = u32le(ph);
        const uint64_t offset = u64le(ph + 8);
        const uint64_t virtual_address = u64le(ph + 16);
        const uint64_t file_bytes = u64le(ph + 32);
        const uint64_t memory_bytes = u64le(ph + 40);

        if (type == PT_DYNAMIC) {
            dynamic_vaddr = virtual_address;
            dynamic_size = file_bytes;
        } else if (type == PT_LOAD) {
            if (segment_count == MAX_LOAD_SEGMENTS ||
                memory_bytes > UINT64_MAX - virtual_address)
                return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid PT_LOAD address range.");
            segments[segment_count++] = (Segment){offset, virtual_address, file_bytes, memory_bytes};
            if (virtual_address < low)
                low = virtual_address;
            if (virtual_address + memory_bytes > high)
                high = virtual_address + memory_bytes;
        }
    }

    uint64_t aligned_high = 0;
    low &= ~(uint64_t)(PAGE_SIZE - 1);
    if (!align_up(high, PAGE_SIZE, &aligned_high) || aligned_high <= low ||
        aligned_high - low > ENGINE_STAGE_MAX_BYTES)
        return abort_stage(stage, ENGINE_STAGE_TOO_LARGE, "ELF image exceeds the staging limit.");

    stage->image_size = (size_t)(aligned_high - low);
    stage->lowest_virtual_address = low;
    stage->image = calloc(1, stage->image_size);
    if (!stage->image)
        return abort_stage(stage, ENGINE_STAGE_IO_ERROR, "Insufficient non-executable staging memory.");

    for (unsigned i = 0; i < segment_count; i++) {
        const Segment *segment = &segments[i];
        unsigned char *destination = mapped_at(stage, segment->virtual_address,
                                                (size_t)segment->memory_bytes);
        if (!destination || !read_at(file, segment->offset, destination,
                                     (size_t)segment->file_bytes))
            return abort_stage(stage, ENGINE_STAGE_IO_ERROR, "Could not stage an ELF segment.");
        // calloc already supplied the zero-filled .bss region.
    }

    unsigned char *dynamic = load_at(stage, segments, segment_count, dynamic_vaddr,
                                     (size_t)dynamic_size, true);
    if (!dynamic)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "PT_DYNAMIC is outside staged memory.");

    uint64_t rela_addr = 0, rela_size = 0, rela_entry = 0;
    uint64_t relr_addr = 0, relr_size = 0, relr_entry = 0;
    uint64_t plt_size = 0, android_packed_size = 0, unsupported_rel = 0;
    bool android_relr = false;
    for (size_t i = 0; i < (size_t)dynamic_size; i += 16) {
        const uint64_t tag = u64le(dynamic + i);
        const uint64_t value = u64le(dynamic + i + 8);
        if (tag == DT_NULL)
            break;
        if (tag == DT_RELA) rela_addr = value;
        else if (tag == DT_RELASZ) rela_size = value;
        else if (tag == DT_RELAENT) rela_entry = value;
        else if (tag == DT_RELR) relr_addr = value;
        else if (tag == DT_RELRSZ) relr_size = value;
        else if (tag == DT_RELRENT) relr_entry = value;
        else if (tag == DT_ANDROID_RELR) { relr_addr = value; android_relr = true; }
        else if (tag == DT_ANDROID_RELRSZ) { relr_size = value; android_relr = true; }
        else if (tag == DT_ANDROID_RELRENT) relr_entry = value;
        else if (tag == DT_PLTRELSZ) plt_size = value;
        else if (tag == DT_JMPREL && value != 0) plt_size = 1;
        else if ((tag == DT_REL || tag == DT_RELSZ || tag == DT_ANDROID_REL ||
                  tag == DT_ANDROID_RELSZ) && value != 0) unsupported_rel = 1;
        else if (tag == DT_ANDROID_RELASZ) android_packed_size = value;
        else if (tag == DT_ANDROID_RELA && value != 0) android_packed_size = 1;
    }

    if (plt_size || android_packed_size || unsupported_rel)
        return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION,
                           "PLT, REL or Android packed symbols need the bionic linker.");
    if (android_relr && relr_size && relr_entry == 0)
        relr_entry = 8; // Older Android RELR tags omit DT_RELRENT.
    if ((rela_size && (rela_entry != 24 || rela_size % 24 != 0)) ||
        (relr_size && (relr_entry != 8 || relr_size % 8 != 0)))
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Malformed relocation table sizes.");
    unsigned char *rela = rela_size ? load_at(stage, segments, segment_count, rela_addr,
                                              (size_t)rela_size, true) : NULL;
    unsigned char *relr = relr_size ? load_at(stage, segments, segment_count, relr_addr,
                                              (size_t)relr_size, true) : NULL;
    if ((rela_size && !rela) || (relr_size && !relr))
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Relocation table is outside staged memory.");

    // Refuse unknown AArch64 relocations before changing a single pointer.
    // External symbols are the job of Cordial's bionic linker, not this layer.
    for (size_t i = 0; i < (size_t)rela_size; i += 24) {
        const uint64_t offset = u64le(rela + i);
        const uint64_t info = u64le(rela + i + 8);
        if ((uint32_t)info != R_AARCH64_RELATIVE || info >> 32 != 0)
            return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION,
                               "An external-symbol relocation requires the bionic linker.");
        if ((offset & 7) != 0 ||
            !load_at(stage, segments, segment_count, offset, 8, false))
            return abort_stage(stage, ENGINE_STAGE_INVALID, "RELA target is outside PT_LOAD.");
    }

    const uint64_t load_bias = (uint64_t)(uintptr_t)stage->image - low;
    for (size_t i = 0; i < (size_t)rela_size; i += 24) {
        const uint64_t target = u64le(rela + i);
        const uint64_t addend = u64le(rela + i + 16);
        write64(load_at(stage, segments, segment_count, target, 8, false), load_bias + addend);
        stage->rela_applied++;
    }

    // RELR encodes one direct address per even word, then a bitmap of the next
    // 63 pointer-sized slots. Android uses this to compress relative relocations.
    uint64_t where = 0;
    bool have_where = false;
    for (size_t i = 0; i < (size_t)relr_size; i += 8) {
        const uint64_t entry = u64le(relr + i);
        if ((entry & 1) == 0) {
            where = entry;
            have_where = true;
            unsigned char *target = load_at(stage, segments, segment_count, where, 8, false);
            if ((where & 7) != 0 || !target)
                return abort_stage(stage, ENGINE_STAGE_INVALID, "RELR address is outside PT_LOAD.");
            write64(target, u64le(target) + load_bias);
            stage->relr_applied++;
            if (where > UINT64_MAX - 8)
                return abort_stage(stage, ENGINE_STAGE_INVALID, "RELR address overflow.");
            where += 8;
        } else {
            if (!have_where || where > UINT64_MAX - 63 * 8)
                return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid RELR bitmap position.");
            for (unsigned bit = 1; bit < 64; bit++) {
                if (!(entry & ((uint64_t)1 << bit)))
                    continue;
                unsigned char *target = load_at(stage, segments, segment_count,
                                                where + (bit - 1) * 8, 8, false);
                if (!target)
                    return abort_stage(stage, ENGINE_STAGE_INVALID,
                                       "RELR bitmap target is outside staged memory.");
                write64(target, u64le(target) + load_bias);
                stage->relr_applied++;
            }
            where += 63 * 8;
        }
    }

    stage->status = ENGINE_STAGE_PREPARED;
    snprintf(stage->detail, sizeof(stage->detail),
             "Staged ELF bytes and relative relocations; not executable.");
    return true;
}
