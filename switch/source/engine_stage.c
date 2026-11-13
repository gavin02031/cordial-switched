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
    DT_SYMTAB = 6,
    DT_STRTAB = 5,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_REL = 17,
    DT_RELSZ = 18,
    DT_RELRSZ = 35,
    DT_RELR = 36,
    DT_RELRENT = 37,
    DT_PLTRELSZ = 2,
    DT_JMPREL = 23,
    DT_PLTREL = 20,
    DT_VERSYM = 0x6ffffff0,
    DT_VERNEED = 0x6ffffffe,
    DT_VERNEEDNUM = 0x6fffffff,
    DT_ANDROID_RELA = 0x60000011,
    DT_ANDROID_RELASZ = 0x60000012,
    DT_ANDROID_REL = 0x6000000f,
    DT_ANDROID_RELSZ = 0x60000010,
    DT_ANDROID_RELR = 0x6fffe000,
    DT_ANDROID_RELRSZ = 0x6fffe001,
    DT_ANDROID_RELRENT = 0x6fffe003,
    R_AARCH64_RELATIVE = 1027,
    R_AARCH64_ABS64 = 257,
    R_AARCH64_GLOB_DAT = 1025,
    R_AARCH64_JUMP_SLOT = 1026,
    R_AARCH64_TLS_DTPMOD = 1028,
    R_AARCH64_TLS_DTPREL = 1029,
    R_AARCH64_TLS_TPREL = 1030,
    R_AARCH64_TLSDESC = 1031,
    PAGE_SIZE = 4096,
    APS2_GROUPED_BY_INFO = 1,
    APS2_GROUPED_BY_OFFSET = 2,
    APS2_GROUPED_BY_ADDEND = 4,
    APS2_HAS_ADDEND = 8,
    APS2_MAX_RELOCATIONS = 8 * 1024 * 1024,
    MAX_VERSION_LIBRARIES = 128,
    MAX_VERSIONS_PER_LIBRARY = 512,
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
    stage->packed_applied = 0;
    stage->symbol_applied = 0;
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

typedef struct {
    uint64_t versym_address;
    uint64_t verneed_address;
    uint64_t verneed_count;
} VersionInfo;

// ELF symbol version indices refer to DT_VERNEED / Elf64_Vernaux, not to
// names in the PLT. Resolve the recorded provider as well as the version so a
// libc import cannot be silently bound to a same-named incompatible function.
static bool resolve_import_version(const EngineStage *stage,
                                   const Segment *segments, unsigned count,
                                   const VersionInfo *versions, uint32_t index,
                                   const unsigned char *strings, size_t strtab_size,
                                   const char **version, const char **library)
{
    *version = NULL;
    *library = NULL;
    if (!versions->versym_address)
        return true;
    if ((uint64_t)index > (UINT64_MAX - versions->versym_address) / 2)
        return false;
    const unsigned char *versym = load_at(stage, segments, count,
                                         versions->versym_address + (uint64_t)index * 2,
                                         2, true);
    if (!versym)
        return false;
    const uint16_t wanted = u16le(versym) & 0x7fff;
    if (wanted < 2)
        return true;
    if (!versions->verneed_address || !versions->verneed_count ||
        versions->verneed_count > MAX_VERSION_LIBRARIES)
        return false;

    uint64_t next = versions->verneed_address;
    for (uint64_t lib = 0; lib < versions->verneed_count; lib++) {
        const unsigned char *need = load_at(stage, segments, count, next, 16, true);
        if (!need || u16le(need) != 1 || !u16le(need + 2) ||
            u16le(need + 2) > MAX_VERSIONS_PER_LIBRARY)
            return false;
        const uint32_t file_offset = u32le(need + 4);
        const uint32_t aux_delta = u32le(need + 8);
        const uint32_t next_delta = u32le(need + 12);
        if (!aux_delta || !file_offset || file_offset >= strtab_size ||
            !strings[file_offset] ||
            !memchr(strings + file_offset, 0, strtab_size - file_offset) ||
            next > UINT64_MAX - aux_delta)
            return false;
        uint64_t aux_address = next + aux_delta;
        for (unsigned i = 0; i < u16le(need + 2); i++) {
            const unsigned char *aux = load_at(stage, segments, count, aux_address,
                                               16, true);
            if (!aux)
                return false;
            const uint32_t name_offset = u32le(aux + 8);
            const uint32_t aux_next = u32le(aux + 12);
            if (!name_offset || name_offset >= strtab_size || !strings[name_offset] ||
                !memchr(strings + name_offset, 0, strtab_size - name_offset))
                return false;
            if ((u16le(aux + 6) & 0x7fff) == wanted) {
                *library = (const char *)strings + file_offset;
                *version = (const char *)strings + name_offset;
                return true;
            }
            if (i + 1 < u16le(need + 2)) {
                if (!aux_next || aux_address > UINT64_MAX - aux_next)
                    return false;
                aux_address += aux_next;
            }
        }
        if (lib + 1 < versions->verneed_count) {
            if (!next_delta || next > UINT64_MAX - next_delta)
                return false;
            next += next_delta;
        }
    }
    return false;
}

static bool apply_rela(EngineStage *stage, const Segment *segments,
                       unsigned count, uint64_t destination, uint64_t info,
                       uint64_t addend, uint64_t load_bias,
                       uint64_t symtab_address, uint64_t sym_entry_size,
                       uint64_t strtab_address, uint64_t strtab_size,
                       const VersionInfo *versions, EngineSymbolResolver resolver,
                       void *context, bool packed, bool audit_missing,
                       unsigned *missing_count)
{
    const uint32_t type = (uint32_t)info;
    const uint32_t sym_index = (uint32_t)(info >> 32);
    const bool tls = type >= R_AARCH64_TLS_DTPMOD && type <= R_AARCH64_TLSDESC;
    // A TLSDESC stores two pointer-sized words. Even in NX audit mode its
    // complete destination must be backed by a real PT_LOAD segment.
    const size_t target_bytes = type == R_AARCH64_TLSDESC ? 16 : 8;
    unsigned char *target = load_at(stage, segments, count, destination, target_bytes, false);
    if ((destination & 7) || !target)
        return abort_stage(stage, ENGINE_STAGE_INVALID,
                           "RELA target is outside PT_LOAD or unaligned.");

    if (type == R_AARCH64_RELATIVE && sym_index == 0) {
        write64(target, load_bias + addend);
        if (packed) stage->packed_applied++;
        else stage->rela_applied++;
        return true;
    }

    if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
        type != R_AARCH64_JUMP_SLOT) {
        if (audit_missing) {
            // Do not invent a DTV, TLS descriptor resolver or any other ABI
            // state. The scratch memory is NX and discarded, so it is safe to
            // retain the unmodified relocation bytes for diagnostic counts.
            if (stage->unsupported_relocations_seen != UINT_MAX)
                stage->unsupported_relocations_seen++;
            if (tls && stage->tls_relocations_seen != UINT_MAX)
                stage->tls_relocations_seen++;
            if (type == R_AARCH64_TLSDESC && stage->tlsdesc_relocations_seen != UINT_MAX)
                stage->tlsdesc_relocations_seen++;
            return true;
        }
        return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION, tls
                           ? "Android TLS relocation requires Bionic DTV/TLS runtime."
                           : "Relocation needs the full Android linker.");
    }
    if (!resolver)
        return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION,
                           "External-symbol relocation needs a symbol resolver.");
    if (!sym_index || sym_entry_size != 24 || !symtab_address || !strtab_address ||
        !strtab_size || (uint64_t)sym_index * 24 > UINT64_MAX - symtab_address)
        return abort_stage(stage, ENGINE_STAGE_INVALID,
                           "Missing or malformed ELF dynamic symbol metadata.");

    const unsigned char *sym = load_at(stage, segments, count,
                                       symtab_address + (uint64_t)sym_index * 24,
                                       24, true);
    const unsigned char *strings = load_at(stage, segments, count,
                                           strtab_address, (size_t)strtab_size, true);
    if (!sym || !strings || u16le(sym + 6) != 0)
        return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION,
                           "Non-imported or invalid dynamic symbol.");

    const uint32_t name_offset = u32le(sym);
    if (name_offset == 0 || name_offset >= strtab_size ||
        !memchr(strings + name_offset, 0, (size_t)(strtab_size - name_offset)))
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid import symbol name.");

    const char *name = (const char *)strings + name_offset;
    const char *version = NULL;
    const char *library = NULL;
    if (!resolve_import_version(stage, segments, count, versions, sym_index,
                                strings, (size_t)strtab_size, &version, &library))
        return abort_stage(stage, ENGINE_STAGE_INVALID,
                           "Malformed or missing Android symbol version metadata.");
    uint64_t symbol_address = 0;
    if (!resolver(name, version, library, &symbol_address, context) || !symbol_address) {
        if (audit_missing) {
            // Leave the unresolved slot zero; the scratch image stays NX and
            // is discarded after the audit. Never synthesize a function or
            // Bionic data-object pointer to make the relocation "succeed".
            write64(target, 0);
            if (*missing_count != UINT_MAX)
                (*missing_count)++;
            return true;
        }
        char message[120];
        if (version)
            snprintf(message, sizeof(message), "Unresolved: %.60s@%.45s", name, version);
        else
            snprintf(message, sizeof(message), "Unresolved import: %.96s", name);
        return abort_stage(stage, ENGINE_STAGE_UNRESOLVED_SYMBOL, message);
    }
    write64(target, symbol_address + addend);
    stage->symbol_applied++;
    if (packed) stage->packed_applied++;
    return true;
}

static bool apply_rela_table(EngineStage *stage, const Segment *segments,
                             unsigned count, const unsigned char *rela,
                             size_t length, uint64_t load_bias,
                             uint64_t symtab_address, uint64_t sym_entry_size,
                             uint64_t strtab_address, uint64_t strtab_size,
                             const VersionInfo *versions, EngineSymbolResolver resolver,
                             void *context, bool audit_missing,
                             unsigned *missing_count)
{
    for (size_t i = 0; i < length; i += 24) {
        if (!apply_rela(stage, segments, count, u64le(rela + i),
                        u64le(rela + i + 8), u64le(rela + i + 16), load_bias,
                        symtab_address, sym_entry_size, strtab_address,
                        strtab_size, versions, resolver, context, false,
                        audit_missing, missing_count))
            return false;
    }
    return true;
}

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t cursor;
} PackedReader;

// APS2 uses signed LEB128 even for the non-negative count, offsets and info.
// Bound every read and reject values that exceed signed 64-bit encoding.
static bool packed_read(PackedReader *reader, int64_t *value)
{
    uint64_t bits = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10; i++, shift += 7) {
        if (reader->cursor >= reader->length)
            return false;
        const unsigned char byte = reader->bytes[reader->cursor++];
        const unsigned char payload = byte & 0x7f;
        if (shift == 63 && payload != 0 && payload != 0x7f)
            return false;
        bits |= (uint64_t)payload << shift;
        if (!(byte & 0x80)) {
            if (byte & 0x40 && shift + 7 < 64)
                bits |= UINT64_MAX << (shift + 7);
            // Preserve the two's-complement bit pattern without an
            // out-of-range unsigned-to-signed integer conversion.
            memcpy(value, &bits, sizeof(bits));
            return true;
        }
    }
    return false;
}

// APS2 offset deltas are SLEB128 values, including negative deltas. Bionic
// adds their bit patterns as unsigned offsets; reject wrapping destinations
// here so the scratch linker never accepts an address outside the ELF image.
static bool packed_advance_offset(uint64_t *offset, int64_t delta)
{
    if (delta < 0) {
        const uint64_t magnitude = 0 - (uint64_t)delta;
        if (*offset < magnitude)
            return false;
        *offset -= magnitude;
    } else {
        if (*offset > UINT64_MAX - (uint64_t)delta)
            return false;
        *offset += (uint64_t)delta;
    }
    return true;
}

// Android's bionic APS2 grouped-RELA encoding (linker_reloc_iterators.h).
// This works ONLY on the non-executable scratch image, never on live code.
static bool apply_packed_rela(EngineStage *stage, const Segment *segments,
                              unsigned count, const unsigned char *data,
                              size_t length, uint64_t load_bias,
                              uint64_t symtab_address, uint64_t sym_entry_size,
                              uint64_t strtab_address, uint64_t strtab_size,
                              const VersionInfo *versions, EngineSymbolResolver resolver,
                              void *context, bool audit_missing,
                              unsigned *missing_count)
{
    if (length < 4 || memcmp(data, "APS2", 4) != 0)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Expected Android APS2 RELA header.");
    PackedReader reader = {data + 4, length - 4, 0};
    int64_t value = 0;
    if (!packed_read(&reader, &value) || value < 0 || value > APS2_MAX_RELOCATIONS)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 relocation count.");
    const uint64_t total = (uint64_t)value;
    if (!packed_read(&reader, &value) || value < 0)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 base offset.");
    uint64_t offset = (uint64_t)value;
    uint64_t info = 0;
    uint64_t addend = 0;

    for (uint64_t consumed = 0; consumed < total;) {
        int64_t size = 0, flags = 0;
        if (!packed_read(&reader, &size) || size <= 0 ||
            (uint64_t)size > total - consumed ||
            !packed_read(&reader, &flags) || flags < 0 || flags > 15 ||
            ((flags & APS2_GROUPED_BY_ADDEND) && !(flags & APS2_HAS_ADDEND)))
            return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 relocation group.");

        int64_t offset_delta = 0;
        if (flags & APS2_GROUPED_BY_OFFSET) {
            if (!packed_read(&reader, &value))
                return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 offset delta.");
            offset_delta = value;
        }
        if (flags & APS2_GROUPED_BY_INFO) {
            if (!packed_read(&reader, &value) || value < 0)
                return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 relocation info.");
            info = (uint64_t)value;
        }
        if (!(flags & APS2_HAS_ADDEND)) {
            addend = 0;
        } else if (flags & APS2_GROUPED_BY_ADDEND) {
            if (!packed_read(&reader, &value))
                return abort_stage(stage, ENGINE_STAGE_INVALID, "Truncated APS2 addend.");
            addend += (uint64_t)value;
        }

        for (uint64_t i = 0; i < (uint64_t)size; i++) {
            if (!(flags & APS2_GROUPED_BY_OFFSET)) {
                if (!packed_read(&reader, &value))
                    return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 offset delta.");
                offset_delta = value;
            }
            if (!packed_advance_offset(&offset, offset_delta))
                return abort_stage(stage, ENGINE_STAGE_INVALID, "APS2 offset overflow.");
            if (!(flags & APS2_GROUPED_BY_INFO)) {
                if (!packed_read(&reader, &value) || value < 0)
                    return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid APS2 relocation info.");
                info = (uint64_t)value;
            }
            if ((flags & APS2_HAS_ADDEND) && !(flags & APS2_GROUPED_BY_ADDEND)) {
                if (!packed_read(&reader, &value))
                    return abort_stage(stage, ENGINE_STAGE_INVALID, "Truncated APS2 addend.");
                addend += (uint64_t)value;
            }
            if (!apply_rela(stage, segments, count, offset, info, addend,
                            load_bias, symtab_address, sym_entry_size,
                            strtab_address, strtab_size, versions,
                            resolver, context, true, audit_missing,
                            missing_count))
                return false;
        }
        consumed += (uint64_t)size;
    }

    if (reader.cursor != reader.length)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Trailing bytes in APS2 stream.");
    return true;
}

static bool engine_stage_stream_internal(FILE *file, EngineStage *stage,
                                         EngineSymbolResolver resolver, void *context,
                                         bool audit_missing)
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
                file_bytes > memory_bytes ||
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

    if (!dynamic_size || dynamic_size % 16)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Invalid PT_DYNAMIC table size.");
    unsigned char *dynamic = load_at(stage, segments, segment_count, dynamic_vaddr,
                                     (size_t)dynamic_size, true);
    if (!dynamic)
        return abort_stage(stage, ENGINE_STAGE_INVALID, "PT_DYNAMIC is outside staged memory.");

    uint64_t rela_addr = 0, rela_size = 0, rela_entry = 0;
    uint64_t relr_addr = 0, relr_size = 0, relr_entry = 0;
    uint64_t plt_size = 0, plt_addr = 0, plt_format = 0;
    uint64_t symtab_address = 0, sym_entry_size = 0, strtab_address = 0, strtab_size = 0;
    uint64_t android_packed_addr = 0, android_packed_size = 0, unsupported_rel = 0;
    bool packed_addr_seen = false, packed_size_seen = false;
    VersionInfo versions = {0};
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
        else if (tag == DT_SYMTAB) symtab_address = value;
        else if (tag == DT_SYMENT) sym_entry_size = value;
        else if (tag == DT_STRTAB) strtab_address = value;
        else if (tag == DT_STRSZ) strtab_size = value;
        else if (tag == DT_ANDROID_RELR) { relr_addr = value; android_relr = true; }
        else if (tag == DT_ANDROID_RELRSZ) { relr_size = value; android_relr = true; }
        else if (tag == DT_ANDROID_RELRENT) relr_entry = value;
        else if (tag == DT_PLTRELSZ) plt_size = value;
        else if (tag == DT_JMPREL) plt_addr = value;
        else if (tag == DT_PLTREL) plt_format = value;
        else if (tag == DT_VERSYM) versions.versym_address = value;
        else if (tag == DT_VERNEED) versions.verneed_address = value;
        else if (tag == DT_VERNEEDNUM) versions.verneed_count = value;
        else if ((tag == DT_REL || tag == DT_RELSZ || tag == DT_ANDROID_REL ||
                  tag == DT_ANDROID_RELSZ) && value != 0) unsupported_rel = 1;
        else if (tag == DT_ANDROID_RELASZ) {
            android_packed_size = value;
            packed_size_seen = true;
        } else if (tag == DT_ANDROID_RELA) {
            android_packed_addr = value;
            packed_addr_seen = true;
        }
    }

    if ((!resolver && (plt_size || plt_addr)) || unsupported_rel)
        return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION,
                           "PLT or unsupported REL needs the bionic linker.");
    if (packed_addr_seen != packed_size_seen ||
        (packed_size_seen && (!android_packed_addr || !android_packed_size ||
                              android_packed_size > ENGINE_STAGE_MAX_BYTES)))
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Malformed Android packed RELA tags.");
    if (android_relr && relr_size && relr_entry == 0)
        relr_entry = 8; // Older Android RELR tags omit DT_RELRENT.
    if ((rela_size && (rela_entry != 24 || rela_size % 24 != 0)) ||
        (plt_size && (!plt_addr || plt_format != DT_RELA || plt_size % 24 != 0)) ||
        (plt_addr && !plt_size) ||
        (relr_size && (relr_entry != 8 || relr_size % 8 != 0)))
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Malformed relocation table sizes.");
    unsigned char *rela = rela_size ? load_at(stage, segments, segment_count, rela_addr,
                                              (size_t)rela_size, true) : NULL;
    unsigned char *relr = relr_size ? load_at(stage, segments, segment_count, relr_addr,
                                              (size_t)relr_size, true) : NULL;
    unsigned char *plt = plt_size ? load_at(stage, segments, segment_count, plt_addr,
                                             (size_t)plt_size, true) : NULL;
    unsigned char *packed = android_packed_size
        ? load_at(stage, segments, segment_count, android_packed_addr,
                  (size_t)android_packed_size, true) : NULL;
    if ((rela_size && !rela) || (relr_size && !relr) || (plt_size && !plt) ||
        (android_packed_size && !packed))
        return abort_stage(stage, ENGINE_STAGE_INVALID, "Relocation table is outside staged memory.");

    // These relocations are applied only to scratch RAM, which remains NX.
    // Unknown symbols and mismatched version requirements fail closed.
    const uint64_t load_bias = (uint64_t)(uintptr_t)stage->image - low;
    unsigned missing_count = 0;
    if ((android_packed_size && !apply_packed_rela(stage, segments, segment_count,
                                                 packed, (size_t)android_packed_size,
                                                 load_bias, symtab_address,
                                                 sym_entry_size, strtab_address,
                                                 strtab_size, &versions, resolver, context,
                                                 audit_missing, &missing_count)) ||
        (rela_size && !apply_rela_table(stage, segments, segment_count, rela,
                                       (size_t)rela_size, load_bias, symtab_address,
                                       sym_entry_size, strtab_address, strtab_size,
                                       &versions, resolver, context, audit_missing,
                                       &missing_count)) ||
        (plt_size && !apply_rela_table(stage, segments, segment_count, plt,
                                      (size_t)plt_size, load_bias, symtab_address,
                                      sym_entry_size, strtab_address, strtab_size,
                                      &versions, resolver, context, audit_missing,
                                      &missing_count)))
        return false;

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

    if (stage->unsupported_relocations_seen) {
        char detail[120];
        snprintf(detail, sizeof(detail),
                 "%u unsupported relocations (%u TLS, %u TLSDESC); %u missing imports.",
                 stage->unsupported_relocations_seen, stage->tls_relocations_seen,
                 stage->tlsdesc_relocations_seen, missing_count);
        return abort_stage(stage, ENGINE_STAGE_UNSUPPORTED_RELOCATION, detail);
    }
    if (missing_count) {
        char detail[120];
        snprintf(detail, sizeof(detail), "%u unresolved imports; scratch audit only.",
                 missing_count);
        return abort_stage(stage, ENGINE_STAGE_UNRESOLVED_SYMBOL, detail);
    }
    stage->status = ENGINE_STAGE_PREPARED;
    snprintf(stage->detail, sizeof(stage->detail),
             "Staged ELF bytes and supported relocations; not executable.");
    return true;
}

bool engine_stage_stream_resolved(FILE *file, EngineStage *stage,
                                  EngineSymbolResolver resolver, void *context)
{
    return engine_stage_stream_internal(file, stage, resolver, context, false);
}

bool engine_stage_stream_audit(FILE *file, EngineStage *stage,
                               EngineSymbolResolver resolver, void *context)
{
    return engine_stage_stream_internal(file, stage, resolver, context, true);
}

bool engine_stage_stream(FILE *file, EngineStage *stage)
{
    return engine_stage_stream_resolved(file, stage, NULL, NULL);
}
