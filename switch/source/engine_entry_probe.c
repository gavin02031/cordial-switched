#include "engine_entry_probe.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    ELF_HEADER_BYTES = 64,
    ELF_PROGRAM_HEADER_BYTES = 56,
    ELF_SECTION_HEADER_BYTES = 64,
    ELF_MACHINE_AARCH64 = 183,
    ELF_TYPE_SHARED = 3,
    SHT_DYNSYM = 11,
    SHT_STRTAB = 3,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PF_X = 1,
    DT_NULL = 0,
    DT_INIT = 12,
    DT_INIT_ARRAY = 25,
    DT_INIT_ARRAYSZ = 27,
    MAX_SECTION_HEADERS = 4096,
    MAX_SYMBOL_COUNT = 262144,
    MAX_STRTAB_BYTES = 16 * 1024 * 1024,
    MAX_INIT_ARRAY_ENTRIES = 8192,
    MAX_LOAD_SEGMENTS = 128
};

typedef struct {
    uint64_t start, end;
    bool executable;
} EntrySegment;

static uint16_t u16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p)
{
    return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}
static bool in_file(uint64_t at, uint64_t bytes, uint64_t length)
{
    return at <= length && bytes <= length - at;
}
static bool read_at(FILE *file, uint64_t at, void *buffer, size_t length)
{
    return at <= LONG_MAX && fseek(file, (long)at, SEEK_SET) == 0 &&
           fread(buffer, 1, length, file) == length;
}
static void fail(EngineEntryProbe *result, EngineEntryProbeStatus status,
                 const char *detail)
{
    result->status = status;
    snprintf(result->detail, sizeof(result->detail), "%s", detail);
}
static bool executable_address(const EntrySegment *segments, unsigned count,
                               uint64_t address)
{
    for (unsigned i = 0; i < count; i++)
        if (segments[i].executable && address >= segments[i].start &&
            address < segments[i].end)
            return true;
    return false;
}

void engine_entry_probe_stream(FILE *file, EngineEntryProbe *result)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot open ARM64 entry metadata.");
        return;
    }
    const long length = ftell(file);
    if (length < ELF_HEADER_BYTES) {
        fail(result, ENGINE_ENTRY_INVALID, "Missing ELF header.");
        return;
    }
    unsigned char header[ELF_HEADER_BYTES];
    if (!read_at(file, 0, header, sizeof(header))) {
        fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot read ELF header.");
        return;
    }
    if (memcmp(header, "\177ELF", 4) || header[4] != 2 || header[5] != 1 ||
        u16(header + 16) != ELF_TYPE_SHARED ||
        u16(header + 18) != ELF_MACHINE_AARCH64) {
        fail(result, ENGINE_ENTRY_INVALID, "Expected little-endian ARM64 ET_DYN.");
        return;
    }
    const uint64_t phoff = u64(header + 32);
    const unsigned phnum = u16(header + 56);
    if (!phnum || phnum > MAX_LOAD_SEGMENTS ||
        u16(header + 54) != ELF_PROGRAM_HEADER_BYTES ||
        !in_file(phoff, (uint64_t)phnum * ELF_PROGRAM_HEADER_BYTES,
                 (uint64_t)length)) {
        fail(result, ENGINE_ENTRY_INVALID, "Invalid ELF program headers.");
        return;
    }

    EntrySegment segments[MAX_LOAD_SEGMENTS];
    unsigned segment_count = 0;
    uint64_t dynamic_offset = 0, dynamic_size = 0;
    for (unsigned i = 0; i < phnum; i++) {
        unsigned char ph[ELF_PROGRAM_HEADER_BYTES];
        if (!read_at(file, phoff + (uint64_t)i * sizeof(ph), ph, sizeof(ph))) {
            fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot read program header.");
            return;
        }
        const uint32_t type = u32(ph);
        const uint64_t offset = u64(ph + 8);
        const uint64_t vaddr = u64(ph + 16);
        const uint64_t filesz = u64(ph + 32);
        const uint64_t memsz = u64(ph + 40);
        if (!in_file(offset, filesz, (uint64_t)length) || memsz < filesz ||
            memsz > UINT64_MAX - vaddr) {
            fail(result, ENGINE_ENTRY_INVALID, "Invalid ELF segment bounds.");
            return;
        }
        if (type == PT_LOAD)
            segments[segment_count++] = (EntrySegment){vaddr, vaddr + memsz,
                                                        (u32(ph + 4) & PF_X) != 0};
        else if (type == PT_DYNAMIC) {
            dynamic_offset = offset;
            dynamic_size = filesz;
        }
    }
    if (!segment_count || !dynamic_size || dynamic_size > 1024 * 1024 ||
        dynamic_size % 16) {
        fail(result, ENGINE_ENTRY_INVALID, "Missing or oversized ELF dynamic section.");
        return;
    }

    uint64_t init_array_size = 0;
    for (uint64_t i = 0; i < dynamic_size / 16; i++) {
        unsigned char entry[16];
        if (!read_at(file, dynamic_offset + i * 16, entry, sizeof(entry))) {
            fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot read dynamic entry.");
            return;
        }
        const uint64_t tag = u64(entry);
        const uint64_t value = u64(entry + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_INIT) {
            result->has_init = value != 0;
            result->init_virtual_address = value;
        } else if (tag == DT_INIT_ARRAY) {
            result->init_array_virtual_address = value;
        } else if (tag == DT_INIT_ARRAYSZ) {
            init_array_size = value;
        }
    }
    if ((result->has_init && !executable_address(segments, segment_count,
                                                 result->init_virtual_address)) ||
        init_array_size % 8 || init_array_size / 8 > MAX_INIT_ARRAY_ENTRIES ||
        ((init_array_size != 0) != (result->init_array_virtual_address != 0))) {
        fail(result, ENGINE_ENTRY_INVALID, "Invalid ELF constructor addresses.");
        return;
    }
    result->init_array_entries = (unsigned)(init_array_size / 8);
    result->has_init_array = init_array_size != 0;

    const uint64_t shoff = u64(header + 40);
    const unsigned shnum = u16(header + 60);
    if (!shoff || !shnum) {
        fail(result, ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS,
             "Section headers stripped; JNI symbol unknown, not absent.");
        return;
    }
    if (shnum > MAX_SECTION_HEADERS ||
        u16(header + 58) != ELF_SECTION_HEADER_BYTES ||
        !in_file(shoff, (uint64_t)shnum * ELF_SECTION_HEADER_BYTES,
                 (uint64_t)length)) {
        fail(result, ENGINE_ENTRY_INVALID, "Invalid section header table.");
        return;
    }

    bool dynsym_seen = false;
    for (unsigned section_index = 0; section_index < shnum; section_index++) {
        unsigned char section[ELF_SECTION_HEADER_BYTES];
        if (!read_at(file, shoff + (uint64_t)section_index * sizeof(section),
                     section, sizeof(section))) {
            fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot read ELF section.");
            return;
        }
        if (u32(section + 4) != SHT_DYNSYM) continue;
        dynsym_seen = true;
        const unsigned strings_index = u32(section + 40);
        const uint64_t symbols_offset = u64(section + 24);
        const uint64_t symbols_size = u64(section + 32);
        if (strings_index >= shnum || u64(section + 56) != 24 ||
            symbols_size % 24 || symbols_size / 24 > MAX_SYMBOL_COUNT ||
            !in_file(symbols_offset, symbols_size, (uint64_t)length)) {
            fail(result, ENGINE_ENTRY_INVALID, "Invalid dynamic symbol table.");
            return;
        }
        unsigned char strings[ELF_SECTION_HEADER_BYTES];
        if (!read_at(file, shoff + (uint64_t)strings_index * sizeof(strings),
                     strings, sizeof(strings)) || u32(strings + 4) != SHT_STRTAB) {
            fail(result, ENGINE_ENTRY_INVALID, "Missing linked ELF string table.");
            return;
        }
        const uint64_t strings_offset = u64(strings + 24);
        const uint64_t strings_size = u64(strings + 32);
        if (strings_size > MAX_STRTAB_BYTES ||
            !in_file(strings_offset, strings_size, (uint64_t)length)) {
            fail(result, ENGINE_ENTRY_INVALID, "Invalid dynamic symbol strings.");
            return;
        }
        for (uint64_t i = 0; i < symbols_size / 24; i++) {
            unsigned char symbol[24];
            if (!read_at(file, symbols_offset + i * 24, symbol, sizeof(symbol))) {
                fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot read dynamic symbol.");
                return;
            }
            const uint32_t name_offset = u32(symbol);
            if (name_offset >= strings_size || u16(symbol + 6) == 0 ||
                (symbol[4] & 0xf) != 2)
                continue; // Undefined symbol or non-function metadata.
            const uint64_t address = u64(symbol + 8);
            if (!executable_address(segments, segment_count, address)) continue;
            char name[32] = {0};
            uint64_t remaining = strings_size - name_offset;
            if (remaining > sizeof(name)) remaining = sizeof(name);
            if (!read_at(file, strings_offset + name_offset, name,
                         (size_t)remaining)) {
                fail(result, ENGINE_ENTRY_IO_ERROR, "Cannot read symbol name.");
                return;
            }
            if (!memchr(name, 0, sizeof(name))) continue;
            if (strcmp(name, "JNI_OnLoad") == 0) {
                result->has_jni_onload = true;
                result->jni_onload_virtual_address = address;
            } else if (strcmp(name, "ANativeActivity_onCreate") == 0) {
                result->has_native_activity_oncreate = true;
                result->native_activity_virtual_address = address;
            }
        }
    }
    result->status = dynsym_seen ? ENGINE_ENTRY_INSPECTED
                                 : ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS;
    snprintf(result->detail, sizeof(result->detail), "%s",
             dynsym_seen ? "ARM64 ELF initialization targets inspected."
                         : "No exported dynamic-symbol section found.");
}
