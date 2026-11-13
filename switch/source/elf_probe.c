#include "elf_probe.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "android_log_compat.h"

enum {
    ELF_HEADER_BYTES = 64,
    PROGRAM_HEADER_BYTES = 56,
    ELF_MACHINE_AARCH64 = 183,
    ELF_TYPE_SHARED_OBJECT = 3,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_TLS = 7,
    MAX_PROGRAM_HEADERS = 128,
    MAX_DYNAMIC_BYTES = 1024 * 1024,
    MAX_NEEDED_OFFSETS = 128,
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_SYMTAB = 6,
    DT_STRTAB = 5,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_PLTREL = 20,
    DT_JMPREL = 23,
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

// Follow only the read-only PLT and dynsym metadata. This is a blocker report,
// not a linker: none of these imported symbols are resolved or called.
static bool inspect_plt_imports(FILE *file, EngineProbeResult *result,
                                 const LoadSegment *segments, unsigned count,
                                 uint64_t plt_address, uint64_t plt_bytes,
                                 uint64_t symtab_address, uint64_t sym_entry_size,
                                 uint64_t versym_address,
                                 uint64_t strtab_file_offset, uint64_t strtab_bytes,
                                 FILE *manifest)
{
    if (!plt_bytes)
        return true;
    if (!plt_address || !symtab_address || sym_entry_size != 24 ||
        plt_bytes % 24 || plt_bytes / 24 > 65536) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "Invalid PLT/symbol metadata.");
        return false;
    }
    uint64_t plt_offset = 0;
    if (!virtual_to_file(segments, count, plt_address, plt_bytes, &plt_offset)) {
        failure(result, ENGINE_PROBE_INVALID_ELF, "PLT relocations are outside PT_LOAD.");
        return false;
    }

    for (uint64_t i = 0; i < plt_bytes / 24; i++) {
        unsigned char entry[8];
        if (!read_at(file, plt_offset + i * 24 + 8, entry, sizeof(entry))) {
            failure(result, ENGINE_PROBE_IO_ERROR, "Could not read PLT relocation.");
            return false;
        }
        const uint64_t info = read64(entry);
        const uint64_t symbol_index = info >> 32;
        if (symbol_index == 0)
            continue;
        if (symbol_index > (UINT64_MAX - symtab_address) / 24) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "Dynamic symbol index overflow.");
            return false;
        }
        uint64_t sym_offset = 0;
        if (!virtual_to_file(segments, count, symtab_address + symbol_index * 24,
                             24, &sym_offset)) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "PLT symbol is outside PT_LOAD.");
            return false;
        }
        unsigned char symbol[24];
        if (!read_at(file, sym_offset, symbol, sizeof(symbol))) {
            failure(result, ENGINE_PROBE_IO_ERROR, "Could not read dynamic symbol.");
            return false;
        }
        const uint32_t name_offset = read32(symbol);
        if (name_offset >= strtab_bytes) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "PLT import name is outside strings.");
            return false;
        }

        // Only undefined symbols require cross-library resolution. The count
        // includes repeats, which the future linker must still bind safely.
        if (read16(symbol + 6) != 0)
            continue;
        result->plt_imports++;

        uint16_t symbol_version = 0;
        if (versym_address) {
            if (symbol_index > (UINT64_MAX - versym_address) / 2) {
                failure(result, ENGINE_PROBE_INVALID_ELF, "Symbol version index overflow.");
                return false;
            }
            uint64_t versym_offset = 0;
            if (!virtual_to_file(segments, count, versym_address + symbol_index * 2,
                                 2, &versym_offset)) {
                failure(result, ENGINE_PROBE_INVALID_ELF, "Symbol version is outside PT_LOAD.");
                return false;
            }
            unsigned char versym[2];
            if (!read_at(file, versym_offset, versym, sizeof(versym))) {
                failure(result, ENGINE_PROBE_IO_ERROR, "Could not read symbol version.");
                return false;
            }
            symbol_version = read16(versym) & 0x7fff;
            if (symbol_version > 1)
                result->plt_versioned_imports++;
        }

        if (result->captured_import_names >= ENGINE_PROBE_MAX_IMPORT_NAMES && !manifest)
            continue;
        char name[192];
        const uint64_t remaining = strtab_bytes - name_offset;
        const size_t size = remaining < sizeof(name) - 1
            ? (size_t)remaining : sizeof(name) - 1;
        if (!read_at(file, strtab_file_offset + name_offset, name, size)) {
            failure(result, ENGINE_PROBE_IO_ERROR, "Could not read PLT import name.");
            return false;
        }
        name[size] = 0;
        // Prevent untrusted ELF strings from drawing control characters.
        for (size_t j = 0; j < size && name[j]; j++) {
            if ((unsigned char)name[j] < 32 || (unsigned char)name[j] > 126)
                name[j] = '?';
        }
        if (!memchr(name, 0, size) && size == sizeof(name) - 1)
            memcpy(name + size - 3, "...", 3);

        if (symbol_version <= 1 && android_log_compat_has(name))
            result->plt_log_compat_imports++;

        if (result->captured_import_names < ENGINE_PROBE_MAX_IMPORT_NAMES) {
            snprintf(result->import_names[result->captured_import_names],
                     sizeof(result->import_names[0]), "%.63s", name);
            result->captured_import_names++;
        }
        if (manifest)
            fprintf(manifest, "%llu\t%u\t%s\n",
                    (unsigned long long)symbol_index, symbol_version, name);
    }
    return true;
}

static void engine_probe_stream_to(FILE *file, EngineProbeResult *result,
                                    FILE *manifest)
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
    uint64_t tls_virtual_address = 0;
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

        if (type == PT_TLS) {
            // PT_TLS is a template for each thread, not executable memory.
            // Never accept a second header that could replace its metadata.
            if (result->has_tls_segment ||
                !inside(file_offset, file_bytes, result->file_bytes) ||
                memory_bytes < file_bytes ||
                virtual_address > UINT64_MAX - memory_bytes ||
                (alignment > 1 &&
                 ((alignment & (alignment - 1)) != 0 ||
                  file_offset % alignment != virtual_address % alignment))) {
                failure(result, ENGINE_PROBE_INVALID_ELF, "Malformed PT_TLS segment.");
                return;
            }
            result->has_tls_segment = true;
            result->tls_initial_image_bytes = file_bytes;
            result->tls_memory_bytes = memory_bytes;
            result->tls_alignment = alignment;
            result->tls_file_offset = file_offset;
            tls_virtual_address = virtual_address;
            continue;
        }

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
            const uint32_t flags = read32(ph + 4);
            result->load[segments_count] = (EngineProbeLoadSegment){
                virtual_address, memory_bytes, flags
            };
            segments[segments_count++] = (LoadSegment){file_offset, virtual_address, file_bytes};
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
    if (result->has_tls_segment) {
        // The zero-filled TLS tail must also belong to one PT_LOAD mapping.
        // Checking only the file-backed initializer misses an out-of-range tail.
        bool contained = false;
        for (unsigned i = 0; i < segments_count; i++) {
            if (tls_virtual_address < result->load[i].virtual_address)
                continue;
            const uint64_t relative = tls_virtual_address -
                result->load[i].virtual_address;
            if (inside(relative, result->tls_memory_bytes,
                       result->load[i].memory_bytes)) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            failure(result, ENGINE_PROBE_INVALID_ELF,
                    "PT_TLS memory is outside PT_LOAD.");
            return;
        }
    }
    if (result->has_tls_segment && result->tls_initial_image_bytes) {
        // A TLS initializer outside the file-backed load segments would be
        // unrepresentable to a future linker even if it were readable.
        uint64_t initializer = 0;
        if (!virtual_to_file(segments, segments_count, tls_virtual_address,
                             result->tls_initial_image_bytes, &initializer)) {
            failure(result, ENGINE_PROBE_INVALID_ELF, "PT_TLS initializer is outside PT_LOAD.");
            return;
        }
    }
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
    uint64_t plt_address = 0, symtab_address = 0, sym_entry_size = 0, versym_address = 0;
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
        else if (tag == DT_JMPREL)
            plt_address = value;
        else if (tag == DT_SYMTAB)
            symtab_address = value;
        else if (tag == DT_SYMENT)
            sym_entry_size = value;
        else if (tag == DT_VERSYM) {
            versym_address = value;
            result->has_symbol_versions |= value != 0;
        }
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

    // Import names come from a real DT_JMPREL table; the synthetic test
    // fixtures in older revisions have no symbol table and remain valid.
    if (plt_bytes && !result->has_unsupported_plt_format &&
        !inspect_plt_imports(file, result, segments, segments_count,
                             plt_address, plt_bytes, symtab_address,
                             sym_entry_size, versym_address,
                             strtab_file_offset, string_bytes, manifest))
        return;

    failure(result, ENGINE_PROBE_INSPECTED, "ARM64 ELF inspected; engine not executed.");
}

void engine_probe_stream(FILE *file, EngineProbeResult *result)
{
    engine_probe_stream_to(file, result, NULL);
}

void engine_probe_stream_manifest(FILE *file, EngineProbeResult *result,
                                  FILE *manifest)
{
    engine_probe_stream_to(file, result, manifest);
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

bool engine_probe_export_imports(const char *path, char *saved_path,
                                  size_t saved_path_bytes)
{
    if (!saved_path || saved_path_bytes < 64 || !path)
        return false;
    saved_path[0] = 0;
    FILE *source = fopen(path, "rb");
    if (!source)
        return false;

    // The first pass validates the metadata before a user-triggered export
    // creates a file. Every report contains names only, never engine bytes.
    EngineProbeResult preview;
    engine_probe_stream(source, &preview);
    if (preview.status != ENGINE_PROBE_INSPECTED) {
        fclose(source);
        return false;
    }

    bool saved = false;
    for (unsigned number = 1; number <= 99 && !saved; number++) {
        char candidate[112];
        snprintf(candidate, sizeof(candidate), ENGINE_REPORT_DIR "/imports-%02u.txt", number);
        FILE *existing = fopen(candidate, "rb");
        if (existing) {
            fclose(existing);
            continue;
        }
        if (errno != ENOENT)
            break;
        FILE *report = fopen(candidate, "wb");
        if (!report)
            break;
        fprintf(report, "# Cordial Switched read-only ARM64 PLT import report\n");
        fprintf(report, "# Symbol index\tVersion index\tImport name\n");
        EngineProbeResult check;
        engine_probe_stream_to(source, &check, report);
        if (check.status == ENGINE_PROBE_INSPECTED)
            fprintf(report, "# COMPLETE: %llu undefined PLT import entries, %llu versioned\n",
                    (unsigned long long)check.plt_imports,
                    (unsigned long long)check.plt_versioned_imports);
        bool ok = check.status == ENGINE_PROBE_INSPECTED && !ferror(report);
        if (fclose(report) != 0)
            ok = false;
        // The diagnostic is allowed to leave an explicitly incomplete file
        // on sudden SD removal; it never deletes or replaces an old report.
        if (ok) {
            const int written = snprintf(saved_path, saved_path_bytes, "%s", candidate);
            saved = written > 0 && (size_t)written < saved_path_bytes;
        }
    }
    fclose(source);
    return saved;
}
