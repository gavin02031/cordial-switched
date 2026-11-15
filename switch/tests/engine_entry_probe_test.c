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
    put64(image + 0x350 + 32, 192);
    put32(image + 0x350 + 40, 2);
    put64(image + 0x350 + 56, 24);

    put32(image + 0x390 + 4, 3); // SHT_STRTAB
    put64(image + 0x390 + 24, 0x900);
    put64(image + 0x390 + 32, 768);
    static const char *const names[] = {
        "JNI_OnLoad",
        "ANativeActivity_onCreate",
        "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode",
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit",
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2InitWithParams",
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeStartLuaAppDM",
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartAppWithParams"
    };
    uint32_t string_offset = 1;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const size_t length = strlen(names[i]) + 1;
        assert(string_offset + length <= 768);
        memcpy(image + 0x900 + string_offset, names[i], length);
        const size_t symbol_offset = 0x240 + (i + 1) * 24;
        put32(image + symbol_offset, string_offset);
        image[symbol_offset + 4] = 2; // STT_FUNC
        put16(image + symbol_offset + 6, 1); // defined in RX segment
        put64(image + symbol_offset + 8, 0x1120 + i * 16);
        string_offset += (uint32_t)length;
    }
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

static void stripped_dynamic_fixture(unsigned char *image, bool gnu)
{
    make_fixture(image);
    put64(image + 40, 0); // Deliberately no ELF section headers.
    put16(image + 60, 0);
    put64(image + 120 + 32, 96);
    put64(image + 120 + 40, 96);
    dynamic(image, 0, 6, 0x1240); // DT_SYMTAB
    dynamic(image, 1, 5, 0x1900); // DT_STRTAB
    dynamic(image, 2, 10, 768); // DT_STRSZ
    dynamic(image, 3, gnu ? 0x6ffffef5 : 4, 0x1700);
    dynamic(image, 4, 11, 24); // DT_SYMENT
    dynamic(image, 5, 0, 0);
    if (gnu) {
        put32(image + 0x700, 1); // GNU bucket count
        put32(image + 0x704, 1); // first hashed symbol index
        put32(image + 0x708, 1); // bloom word count
        put32(image + 0x718, 1); // bucket starts at index 1
        put32(image + 0x71c, 0); // first chain element
        put32(image + 0x720, 0);
        put32(image + 0x724, 0);
        put32(image + 0x728, 0);
        put32(image + 0x72c, 0);
        put32(image + 0x730, 0);
        put32(image + 0x734, 1); // last hashed symbol (#7)
    } else {
        put32(image + 0x700, 1); // SysV bucket count
        put32(image + 0x704, 8); // SysV symbol count
    }
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
    assert(result.has_game_activity_initialize_native &&
           result.game_activity_initialize_native_virtual_address == 0x1140);
    assert(result.has_app_bridge_global_init &&
           result.app_bridge_global_init_virtual_address == 0x1150);
    assert(result.has_app_bridge_v2_init &&
           result.app_bridge_v2_init_virtual_address == 0x1160);
    assert(result.has_app_bridge_lua_shell_start &&
           result.app_bridge_lua_shell_start_virtual_address == 0x1170);
    assert(result.has_app_bridge_start_app &&
           result.app_bridge_start_app_virtual_address == 0x1180);
    assert(result.has_init && result.init_virtual_address == 0x1100);
    assert(result.has_init_array && result.init_array_entries == 2);
    assert(result.init_array_virtual_address == 0x1200);

    // Android libraries frequently strip section headers while preserving
    // PT_DYNAMIC; both standard dynamic hash formats must still be inspectable.
    stripped_dynamic_fixture(fixture, false);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED);
    assert(result.has_jni_onload && result.has_native_activity_oncreate);
    assert(result.has_game_activity_initialize_native &&
           result.game_activity_initialize_native_virtual_address == 0x1140);
    assert(result.has_app_bridge_start_app);
    stripped_dynamic_fixture(fixture, true);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED);
    assert(result.has_game_activity_initialize_native);
    assert(result.has_app_bridge_v2_init && result.has_app_bridge_lua_shell_start);

    // The correct exported symbol must be defined in executable PT_LOAD.
    put64(fixture + 0x2e8 + 8, 0x900000);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED &&
           !result.has_app_bridge_start_app && result.has_app_bridge_v2_init);
    stripped_dynamic_fixture(fixture, true);

    // Reject exported function addresses outside executable load segments.
    put64(fixture + 0x288 + 8, 0x900000);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED &&
           !result.has_game_activity_initialize_native);

    // Malformed or incomplete hash metadata must remain unknown, not absent.
    stripped_dynamic_fixture(fixture, true);
    put32(fixture + 0x718, 0xffffffff);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS);
    stripped_dynamic_fixture(fixture, false);
    put32(fixture + 0x704, 0xffffffff);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS);

    make_fixture(fixture);

    // Do not mistake a missing section table for an absent JNI entry.
    put64(fixture + 40, 0);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS);
    assert(!result.has_jni_onload && !result.has_game_activity_initialize_native &&
           result.has_init);
    make_fixture(fixture);

    // Undefined symbols are imports, not engine entry points.
    put16(fixture + 0x258 + 6, 0);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED && !result.has_jni_onload);
    make_fixture(fixture);

    // A truncated string entry cannot masquerade as a complete export name.
    put64(fixture + 0x390 + 32, 37 +
          strlen("Java_com_google_androidgamesdk_GameActivity_initializeNativeCode"));
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED &&
           !result.has_game_activity_initialize_native);
    make_fixture(fixture);

    // A named function outside RX load segments is not an executable entry.
    put64(fixture + 0x288 + 8, 0x900000);
    result = run_fixture(fixture);
    assert(result.status == ENGINE_ENTRY_INSPECTED &&
           !result.has_game_activity_initialize_native);
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
