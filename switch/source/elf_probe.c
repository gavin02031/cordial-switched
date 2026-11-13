#include "elf_probe.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    ELF_HEADER_BYTES = 64,
    PROGRAM_HEADER_BYTES = 56,
    ELF_MACHINE_AARCH64 = 183,
    ELF_TYPE_SHARED_OBJECT = 3,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    MAX_PROGRAM_HEADERS = 128,
    MAX_DYNAMIC_BYTES = 1024 * 1024,
    MAX_NEEDED_OFFSETS = 128,
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_STRTAB = 5,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_PLTREL = 20,
    DT_RELRSZ = 35,
    DT_ANDROID_RELASZ = 0x60000012,
    DT_ANDROID_RELSZ = 0x60000010,
    DT_ANDROID_RELRSZ = 0x6fffe001,
    DT_VERSYM = 0x6ffffff0,
    DT_VERDEF = 0x6ffffffc,
    DT_VERNEED = 0x6ffffffe,
};

typedef struct {
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_bytes;
} LoadSegment;

static uint16_t read16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const unsigned char *p)
{
    return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

static bool inside(uint64_t offset, uint64_t size, uint64_t end)
{
    return offset <= end && size <= end - offset;
}

static bool read_at(FILE *file, uint64_t offset, void *buffer, size_t size)
{
    if (offset > LONG_MAX)
        return false;
    return fseek(file, (long)offset, SEEK_SET) == 0 &&
           fread(buffer, 1, size, file) == size;
}

static void failure(EngineProbeResult *result, EngineProbeStatus status,
                    const char *reason)
{
    result->status = status;
    snprintf(result->detail, sizeof(result->detail), "%s", reason);
}

static bool virtual_to_file(const LoadSegment *segments, unsigned count,
                            uint64_t virtual_address, uint64_t size,
                            uint64_t *file_offset)
{
    for (unsigned i = 0; i < count; i++) {
        const LoadSegment *segment = &segments[i];
        if (virtual_address < segment->virtual_address)
            continue;
        const uint64_t relative = virtual_address - segment->virtual_address;
        if (!inside(relative, size, segment->file_bytes))
            continue;
        if (segment->file_offset > UINT64_MAX - relative)
            return false;
        *file_offset = segment->file_offset + relative;
        return true;
    }
    return false;
}

void engine_probe_stream(FILE *file, EngineProbeResult *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof(*result));
    failure(result, ENGINE_PROBE_IO_ERROR, "Could not read the library.");
    if (!file || fseek(file, 0, SEEK_END) != 0)
        return;

    const long length = ftell(file);
    if (length < 0)
        return;
    result->file_bytes = (uint64_t)length;
    if (result->file_bytes < ELF_HEADER_BYTES) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "File is too short for ELF64.");
        return;
    }

    unsigned char header[ELF_HEADER_BYTES];
    if (!read_at(file, 0, header, sizeof(header)))
        return;
    if (memcmp(header, "\x7f" "ELF", 4) != 0 ||
        header[4] != 2 || header[5] != 1 || header[6] != 1) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Not a little-endian ELF64 library.");
        return;
    }
    if (read16(header + 18) != ELF_MACHINE_AARCH64) {
        failure(result, ENGINE_PROBE_WRONG_ARCH, "Library is not ARM64 (AArch64).");
        return;
    }
    if (read16(header + 16) != ELF_TYPE_SHARED_OBJECT) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "ARM64 file is not an ELF shared object.");
        return;
    }

    const uint64_t ph_offset = read64(header + 32);
    const unsigned ph_size = read16(header + 54);
    const unsigned ph_count = read16(header + 56);
    if (read16(header + 52) != ELF_HEADER_BYTES ||
        ph_size != PROGRAM_HEADER_BYTES || ph_count == 0 ||
        ph_count > MAX_PROGRAM_HEADERS ||
        !inside(ph_offset, (uint64_t)ph_size * ph_count, result->file_bytes)) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Invalid ELF program-header table.");
        return;
    }

    LoadSegment segments[MAX_PROGRAM_HEADERS];
    unsigned segments_count = 0;
    uint64_t lowest_load = UINT64_MAX;
    uint64_t highest_load = 0;
    uint64_t dynamic_offset = 0;
    uint64_t dynamic_size = 0;
    unsigned char ph[PROGRAM_HEADER_BYTES];
    for (unsigned i = 0; i < ph_count; i++) {
        if (!read_at(file, ph_offset + (uint64_t)i * ph_size, ph, sizeof(ph))) {
            failure(result, ENGINE_PROBE_IO_ERROR, "Could not read ELF program headers.");
            return;
        }

        const uint32_t type = read32(ph);
        const uint64_t file_offset = read64(ph + 8);
        const uint64_t virtual_address = read64(ph + 16);
        const uint64_t file_bytes = read64(ph + 32);
        const uint64_t memory_bytes = read64(ph + 40);
        const uint64_t alignment = read64(ph + 48);

        if (type != PT_LOAD && type != PT_DYNAMIC)
            continue;
        if (!inside(file_offset, file_bytes, result->file_bytes) ||
            memory_bytes < file_bytes ||
            virtual_address > UINT64_MAX - memory_bytes ||
            (alignment > 1 &&
             ((alignment & (alignment - 1)) != 0 ||
              file_offset % alignment != virtual_address % alignment))) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "Invalid loadable ELF segment.");
            return;
        }

        if (type == PT_LOAD) {
            segments[segments_count++] = (LoadSegment){file_offset, virtual_address, file_bytes};
            const uint32_t flags = read32(ph + 4);
            if (flags & 1)
                result->executable_segments++;
            if (flags & 2)
                result->writable_segments++;
            if (virtual_address < lowest_load)
                lowest_load = virtual_address;
            if (virtual_address + memory_bytes > highest_load)
                highest_load = virtual_address + memory_bytes;
            if (alignment > result->max_segment_alignment)
                result->max_segment_alignment = alignment;
        } else {
            if (dynamic_size != 0) {
                failure(result, ENGINE_PROBE_INVALID_ELF, "Multiple PT_DYNAMIC segments.");
                return;
            }
            dynamic_offset = file_offset;
            dynamic_size = file_bytes;
        }
    }

    result->load_segments = segments_count;
    if (segments_count)
        result->virtual_image_bytes = highest_load - lowest_load;
    if (segments_count == 0 || dynamic_size == 0 ||
        dynamic_size > MAX_DYNAMIC_BYTES || dynamic_size % 16 != 0) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Missing or malformed dynamic ELF segments.");
        return;
    }

    unsigned char *dynamic = malloc((size_t)dynamic_size);
    if (!dynamic) {
        failure(result, ENGINE_PROBE_IO_ERROR, "Insufficient memory to inspect PT_DYNAMIC.");
        return;
    }
    if (!read_at(file, dynamic_offset, dynamic, (size_t)dynamic_size)) {
        free(dynamic);
        failure(result, ENGINE_PROBE_IO_ERROR, "Could not read ELF dynamic entries.");
        return;
    }

    uint64_t string_virtual_address = 0;
    uint64_t string_bytes = 0;
    uint64_t needed_offsets[MAX_NEEDED_OFFSETS];
    unsigned offsets_count = 0;
    bool terminated = false;
    uint64_t rela_bytes = 0, rela_entry_size = 0, plt_bytes = 0, plt_kind = 0;
    uint64_t relr_bytes = 0, packed_rela = 0, packed_rel = 0;
    for (size_t i = 0; i < (size_t)dynamic_size; i += 16) {
        const uint64_t tag = read64(dynamic + i);
        const uint64_t value = read64(dynamic + i + 8);
        if (tag == DT_NULL) {
            terminated = true;
            break;
        }
        if (tag == DT_STRTAB)
            string_virtual_address = value;
        else if (tag == DT_STRSZ)
            string_bytes = value;
        else if (tag == DT_RELASZ)
            rela_bytes = value;
        else if (tag == DT_RELAENT)
            rela_entry_size = value;
        else if (tag == DT_PLTRELSZ)
            plt_bytes = value;
        else if (tag == DT_PLTREL)
            plt_kind = value;
        else if (tag == DT_RELRSZ || tag == DT_ANDROID_RELRSZ)
            relr_bytes = value;
        else if (tag == DT_ANDROID_RELASZ)
            packed_rela = value;
        else if (tag == DT_ANDROID_RELSZ)
            packed_rel = value;
        else if ((tag == DT_VERSYM || tag == DT_VERDEF || tag == DT_VERNEED) && value)
            result->has_symbol_versions = true;
        else if (tag == DT_NEEDED) {
            if (offsets_count == MAX_NEEDED_OFFSETS) {
                free(dynamic);
                failure(result, ENGINE_PROBE_INVALID_ELF, "Unreasonable DT_NEEDED count.");
                return;
            }
            needed_offsets[offsets_count++] = value;
        }
    }
    free(dynamic);

    // Report declarative relocation sizes only. No code, relocations, or
    // pointers from the user-provided file are executed or followed here.
    result->rela_records = rela_bytes / 24;
    result->plt_records = plt_bytes / 24;
    result->relr_words = relr_bytes / 8;
    result->android_packed_bytes = packed_rela > UINT64_MAX - packed_rel
        ? UINT64_MAX : packed_rela + packed_rel;
    result->has_unsupported_plt_format = (plt_bytes && plt_kind != DT_RELA) ||
        (rela_bytes && rela_entry_size != 24) || rela_bytes % 24 ||
        plt_bytes % 24 || relr_bytes % 8;

    if (!terminated) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Dynamic entries have no DT_NULL terminator.");
        return;
    }

    if (string_virtual_address == 0 || string_bytes == 0) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Missing dynamic string table.");
        return;
    }
    uint64_t strtab_file_offset = 0;
    if (!virtual_to_file(segments, segments_count, string_virtual_address,
                         string_bytes, &strtab_file_offset)) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Dynamic strings are outside PT_LOAD.");
        return;
    }

    result->needed_count = offsets_count;
    for (unsigned i = 0; i < offsets_count; i++) {
        if (needed_offsets[i] >= string_bytes) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "DT_NEEDED string offset is invalid.");
            return;
        }
        // All names are validated, even when only the first five fit on screen.
        char name[sizeof(result->needed[0])];
        const uint64_t remaining = string_bytes - needed_offsets[i];
        const size_t read_size = remaining < sizeof(name) ? (size_t)remaining : sizeof(name);
        if (!read_at(file, strtab_file_offset + needed_offsets[i], name, read_size) ||
            !memchr(name, 0, read_size)) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "Unterminated DT_NEEDED library name.");
            return;
        }
        if (i < ENGINE_PROBE_MAX_NEEDED)
            snprintf(result->needed[i], sizeof(result->needed[i]), "%s", name);
    }

    failure(result, ENGINE_PROBE_INSPECTED, "ARM64 ELF inspected; engine not executed.");
}

void engine_probe_file(const char *path, EngineProbeResult *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof(*result));
    if (!path) {
        failure(result, ENGINE_PROBE_IO_ERROR, "No engine file path was provided.");
        return;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT)
            failure(result, ENGINE_PROBE_NOT_FOUND, "No user-supplied ARM64 library found.");
        else
            failure(result, ENGINE_PROBE_IO_ERROR, "Could not open the engine file.");
        return;
    }
    engine_probe_stream(file, result);
    fclose(file);
}
