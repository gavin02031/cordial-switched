#include "engine_entry_probe.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { FIXTURE_SIZE = 4096 };

static void put16(unsigned char *p, uint16_t n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}
static void put32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8 * i));
}
static void put64(unsigned char *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(n >> (8 * i));
}
static void dynamic(unsigned char *image, unsigned index, uint64_t tag,
                    uint64_t value)
{
    put64(image + 0x180 + index * 16, tag);
    put64(image + 0x188 + index * 16, value);
}

static void make_fixture(unsigned char *image)
{
    memset(image, 0, FIXTURE_SIZE);
    memcpy(image, "\177ELF", 4);
    image[4] = 2; // ELF64
    image[5] = 1; // little endian
    image[6] = 1; // current ELF version
    put16(image + 16, 3); // ET_DYN
    put16(image + 18, 183); // AArch64
    put32(image + 20, 1);
    put64(image + 32, 64); // ELF program headers
    put64(image + 40, 0x310); // ELF section headers
    put16(image + 52, 64);
    put16(image + 54, 56);
    put16(image + 56, 2);
    put16(image + 58, 64);
    put16(image + 60, 3);

    // Synthetic file-backed RX segment; no code or proprietary game data.
    put32(image + 64, 1); // PT_LOAD
    put32(image + 68, 5); // PF_R | PF_X
    put64(image + 64 + 16, 0x1000);
    put64(image + 64 + 32, FIXTURE_SIZE);
    put64(image + 64 + 40, FIXTURE_SIZE);

    put32(image + 120, 2); // PT_DYNAMIC
    put32(image + 124, 4); // PF_R
    put64(image + 120 + 8, 0x180);
    put64(image + 120 + 16, 0x1180);
    put64(image + 120 + 32, 64);
    put64(image + 120 + 40, 64);

    dynamic(image, 0, 12, 0x1100); // DT_INIT
    dynamic(image, 1, 25, 0x1200); // DT_INIT_ARRAY
    dynamic(image, 2, 27, 16); // DT_INIT_ARRAYSZ (2 entries)
    dynamic(image, 3, 0, 0); // DT_NULL
    put64(image + 0x200, 0x1150);
    put64(image + 0x208, 0x1160);

    // Section #1: .dynsym -> linked string table #2.
    put32(image + 0x350 + 4, 11);
    put64(image + 0x350 + 24, 0x240);
    put64(image + 0x350 + 32, 72);
    put32(image + 0x350 + 40, 2);
    put64(image + 0x350 + 56, 24);

    put32(image + 0x390 + 4, 3); // SHT_STRTAB
    put64(image + 0x390 + 24, 0x290);
    put64(image + 0x390 + 32, 64);
    memcpy(image + 0x291, "JNI_OnLoad", 11);
    memcpy(image + 0x29c, "ANativeActivity_onCreate", 25);

    put32(image + 0x258, 1); // .dynsym #1 name
    image[0x258 + 4] = 2; // STT_FUNC
    put16(image + 0x258 + 6, 1); // defined section
    put64(image + 0x258 + 8, 0x1120);
    put32(image + 0x270, 12); // .dynsym #2 name
    image[0x270 + 4] = 2;
    put16(image + 0x270 + 6, 1);
    put64(image + 0x270 + 8, 0x1130);
}

static EngineEntryProbe run_fixture(const unsigned char *image)
{
    FILE *f = tmpfile();
    assert(f);
    assert(fwrite(image, 1, FIXTURE_SIZE, f) == FIXTURE_SIZE);
    EngineEntryProbe result;
    engine_entry_probe_stream(f, &result);
    assert(fclose(f) == 0);
    return result;
}

int main(void)
{
    unsigned char fixture[FIXTURE_SIZE];
    make_fixture(fixture);
    EngineEntryProbe result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED);
    assert(result.has_jni_onload && result.jni_onload_virtual_address == 0x1120);
    assert(result.has_native_activity_oncreate &&
           result.native_activity_virtual_address == 0x1130);
    assert(result.has_init && result.init_virtual_address == 0x1100);
    assert(result.has_init_array && result.init_array_entries == 2);
    assert(result.init_array_virtual_address == 0x1200);

    // Do not mistake a missing section table for an absent JNI entry.
    put64(fixture + 40, 0);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS);
    assert(!result.has_jni_onload && result.has_init);
    make_fixture(fixture);

    // Undefined symbols are imports, not engine entry points.
    put16(fixture + 0x258 + 6, 0);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED && !result.has_jni_onload);
    make_fixture(fixture);

    // An exported symbol in a non-executable segment is not callable.
    put32(fixture + 68, 4);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INVALID);
    make_fixture(fixture);

    // Check dynamic constructor alignment, target range and ELF section bounds.
    dynamic(fixture, 2, 27, 3);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INVALID);
    make_fixture(fixture);
    dynamic(fixture, 0, 12, 0x900000);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INVALID);
    make_fixture(fixture);
    put64(fixture + 0x350 + 24, FIXTURE_SIZE - 8);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INVALID);
    make_fixture(fixture);
    put16(fixture + 18, 62); // x86_64 is not ARM64
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INVALID);
    puts("engine_entry_probe_test: all assertions passed");
    return 0;
}
