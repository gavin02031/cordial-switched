#include "engine_stage.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { IMAGE_BYTES = 1024 };

static void put16(unsigned char *p, uint16_t n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static void put32(unsigned char *p, uint32_t n)
{
    put16(p, (uint16_t)n);
    put16(p + 2, (uint16_t)(n >> 16));
}

static void put64(unsigned char *p, uint64_t n)
{
    put32(p, (uint32_t)n);
    put32(p + 4, (uint32_t)(n >> 32));
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}

static void add_dynamic(unsigned char *p, unsigned index, uint64_t tag, uint64_t value)
{
    put64(p + index * 16, tag);
    put64(p + index * 16 + 8, value);
}

static void fixture(unsigned char *bytes)
{
    memset(bytes, 0, IMAGE_BYTES);
    memcpy(bytes, "\x7f" "ELF", 4);
    bytes[4] = 2;
    bytes[5] = 1;
    bytes[6] = 1;
    put16(bytes + 16, 3); // ET_DYN
    put16(bytes + 18, 183); // EM_AARCH64
    put32(bytes + 20, 1);
    put64(bytes + 32, 64);
    put16(bytes + 52, 64);
    put16(bytes + 54, 56);
    put16(bytes + 56, 2);

    unsigned char *load = bytes + 64;
    put32(load, 1);
    put64(load + 8, 0);
    put64(load + 16, 0x1000);
    put64(load + 32, IMAGE_BYTES);
    put64(load + 40, 0x600); // 512 bytes of zero-filled .bss.
    put64(load + 48, 0x1000);

    unsigned char *dyn_ph = bytes + 64 + 56;
    put32(dyn_ph, 2);
    put64(dyn_ph + 8, 0x180);
    put64(dyn_ph + 16, 0x1180);
    put64(dyn_ph + 32, 10 * 16);
    put64(dyn_ph + 40, 10 * 16);
    put64(dyn_ph + 48, 8);

    unsigned char *dyn = bytes + 0x180;
    add_dynamic(dyn, 0, 5, 0x1300);  // DT_STRTAB
    add_dynamic(dyn, 1, 10, 24);     // DT_STRSZ
    add_dynamic(dyn, 2, 1, 1);       // DT_NEEDED
    add_dynamic(dyn, 3, 7, 0x1240);  // DT_RELA
    add_dynamic(dyn, 4, 8, 24);      // DT_RELASZ
    add_dynamic(dyn, 5, 9, 24);      // DT_RELAENT
    add_dynamic(dyn, 6, 36, 0x12a0); // DT_RELR
    add_dynamic(dyn, 7, 35, 16);    // DT_RELRSZ
    add_dynamic(dyn, 8, 37, 8);     // DT_RELRENT

    put64(bytes + 0x240, 0x1340);  // r_offset
    put64(bytes + 0x248, 1027);    // R_AARCH64_RELATIVE, symbol 0
    put64(bytes + 0x250, 0x1380);  // r_addend

    put64(bytes + 0x2a0, 0x1350);  // RELR direct pointer
    put64(bytes + 0x2a8, 0xb);     // Bitmap: next and third slots
    put64(bytes + 0x350, 0x13a0);
    put64(bytes + 0x358, 0x13b0);
    put64(bytes + 0x368, 0x13c0);
    memcpy(bytes + 0x300, "\0libc.so\0", 9);
}

static void symbol_fixture(unsigned char *bytes, uint32_t type, bool plt)
{
    fixture(bytes);
    unsigned char *dyn = bytes + 0x180;
    add_dynamic(dyn, 2, 6, 0x12c0); // DT_SYMTAB
    if (plt) {
        add_dynamic(dyn, 3, 11, 24);     // DT_SYMENT
        add_dynamic(dyn, 4, 2, 24);      // DT_PLTRELSZ
        add_dynamic(dyn, 5, 23, 0x1240); // DT_JMPREL
        add_dynamic(dyn, 6, 20, 7);      // DT_PLTREL = DT_RELA
    } else {
        add_dynamic(dyn, 6, 11, 24);     // DT_SYMENT
    }
    add_dynamic(dyn, 7, 0, 0);
    add_dynamic(dyn, 8, 0, 0);

    put64(bytes + 0x248, ((uint64_t)1 << 32) | type);
    put64(bytes + 0x250, 8); // Addend for S + A.
    put32(bytes + 0x2d8, 1); // Symbol #1 st_name.
    bytes[0x2dc] = 0x12; // Global function.
    memset(bytes + 0x300, 0, 24);
    memcpy(bytes + 0x301, "sample_symbol", 14);
}

static size_t append_sleb(unsigned char *buffer, size_t at, int64_t number)
{
    for (;;) {
        unsigned char byte = (unsigned char)number & 0x7f;
        number >>= 7;
        const bool done = (number == 0 && !(byte & 0x40)) ||
                          (number == -1 && (byte & 0x40));
        buffer[at++] = byte | (done ? 0 : 0x80);
        if (done)
            return at;
    }
}

typedef struct {
    size_t length;
    size_t group_size_at;
    size_t flags_at;
} PackedFixture;

// Three relative relocations into 0x1340, 0x1350 and 0x1360, with
// Android APS2 grouped offset/info/addend flags. No proprietary code.
static PackedFixture packed_fixture(unsigned char *bytes, int64_t info,
                                    int64_t offset_delta, int64_t addend)
{
    fixture(bytes);
    unsigned char *dyn = bytes + 0x180;
    unsigned char *packed = bytes + 0x2b0;
    memcpy(packed, "APS2", 4);
    size_t at = 4;
    at = append_sleb(packed, at, 3); // Total relocations
    at = append_sleb(packed, at, 0x1330); // Starting virtual offset
    const size_t group_size_at = at;
    at = append_sleb(packed, at, 3); // One group of three
    const size_t flags_at = at;
    at = append_sleb(packed, at, 15); // All four grouped RELA flags
    at = append_sleb(packed, at, offset_delta);
    at = append_sleb(packed, at, info);
    at = append_sleb(packed, at, addend);
    assert(at < 0x300 - 0x2b0);
    add_dynamic(dyn, 3, 0x60000011, 0x12b0); // DT_ANDROID_RELA
    add_dynamic(dyn, 4, 0x60000012, at);     // DT_ANDROID_RELASZ
    add_dynamic(dyn, 5, 0, 0); // No other relocation sections
    return (PackedFixture){at, group_size_at, flags_at};
}

static void ungrouped_packed_fixture(unsigned char *bytes)
{
    fixture(bytes);
    unsigned char *packed = bytes + 0x2b0;
    memcpy(packed, "APS2", 4);
    size_t at = 4;
    at = append_sleb(packed, at, 3);
    at = append_sleb(packed, at, 0x1330);
    at = append_sleb(packed, at, 3);
    at = append_sleb(packed, at, 8); // HAS_ADDEND, everything else ungrouped.
    for (unsigned i = 0; i < 3; i++) {
        at = append_sleb(packed, at, 16); // Offset delta.
        at = append_sleb(packed, at, 1027); // R_AARCH64_RELATIVE.
        at = append_sleb(packed, at, i == 0 ? -8 : i == 1 ? 8 : 16);
    }
    assert(at < 0x300 - 0x2b0);
    add_dynamic(bytes + 0x180, 3, 0x60000011, 0x12b0);
    add_dynamic(bytes + 0x180, 4, 0x60000012, at);
    add_dynamic(bytes + 0x180, 5, 0, 0);
}

static bool fake_resolver(const char *name, uint64_t *address, void *context)
{
    if (strcmp(name, "sample_symbol") != 0 || !context)
        return false;
    *address = *(uint64_t *)context;
    return true;
}

static bool stage_fixture(const unsigned char *bytes, size_t length, EngineStage *stage)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    const bool ok = engine_stage_stream(file, stage);
    fclose(file);
    return ok;
}

static bool resolve_fixture(const unsigned char *bytes, EngineStage *stage,
                            EngineSymbolResolver lookup, void *context)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(bytes, 1, IMAGE_BYTES, file) == IMAGE_BYTES);
    const bool ok = engine_stage_stream_resolved(file, stage, lookup, context);
    fclose(file);
    return ok;
}

int main(void)
{
    unsigned char bytes[IMAGE_BYTES];
    fixture(bytes);
    EngineStage stage;
    assert(stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_PREPARED);
    assert(stage.image_size == 4096);
    assert(stage.lowest_virtual_address == 0x1000);
    assert(stage.rela_applied == 1);
    assert(stage.relr_applied == 3);
    assert(get64(stage.image + 0x340) == (uint64_t)(uintptr_t)(stage.image + 0x380));
    assert(get64(stage.image + 0x350) == (uint64_t)(uintptr_t)(stage.image + 0x3a0));
    assert(get64(stage.image + 0x358) == (uint64_t)(uintptr_t)(stage.image + 0x3b0));
    assert(get64(stage.image + 0x368) == (uint64_t)(uintptr_t)(stage.image + 0x3c0));
    assert(stage.image[0x400] == 0); // BSS is zero-filled.
    engine_stage_release(&stage);
    assert(stage.image == NULL);

    PackedFixture pack = packed_fixture(bytes, 1027, 16, 0x1380);
    assert(stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_PREPARED);
    assert(stage.packed_applied == 3);
    assert(stage.rela_applied == 0);
    assert(stage.relr_applied == 0);
    for (unsigned i = 0; i < 3; i++)
        assert(get64(stage.image + 0x340 + i * 16) ==
               (uint64_t)(uintptr_t)(stage.image + 0x380));
    engine_stage_release(&stage);

    packed_fixture(bytes, 1027, 16, -32); // Signed addends use SLEB128.
    assert(stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.packed_applied == 3);
    assert(get64(stage.image + 0x340) == (uint64_t)(uintptr_t)stage.image - 0x1020);
    engine_stage_release(&stage);

    ungrouped_packed_fixture(bytes);
    assert(stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.packed_applied == 3);
    const uint64_t bias = (uint64_t)(uintptr_t)stage.image - 0x1000;
    assert(get64(stage.image + 0x340) == bias - 8);
    assert(get64(stage.image + 0x350) == bias);
    assert(get64(stage.image + 0x360) == bias + 16);
    engine_stage_release(&stage);

    packed_fixture(bytes, 1025, 16, 0); // Imported symbol, no resolver.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);
    assert(stage.image == NULL);

    pack = packed_fixture(bytes, 1027, 16, 0x1380);
    bytes[0x2b0] = 'X'; // Not APS2.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    pack = packed_fixture(bytes, 1027, 16, 0x1380);
    add_dynamic(bytes + 0x180, 4, 0x60000012, pack.length - 1); // Truncated.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    pack = packed_fixture(bytes, 1027, 16, 0x1380);
    bytes[0x2b0 + pack.flags_at] = 0x10; // Unknown group flag.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    pack = packed_fixture(bytes, 1027, 16, 0x1380);
    bytes[0x2b0 + pack.group_size_at] = 0; // Empty group would not progress.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    pack = packed_fixture(bytes, 1027, 16, 0x1380);
    bytes[0x2b0 + pack.length] = 0; // Noncanonical trailing bytes.
    add_dynamic(bytes + 0x180, 4, 0x60000012, pack.length + 1);
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    packed_fixture(bytes, 1027, 16, 0x1380);
    memset(bytes + 0x2b0 + 4, 0x80, 11); // Overflowing/unterminated SLEB128.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    packed_fixture(bytes, 1027, -16, 0x1380); // Signed negative offset delta.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    packed_fixture(bytes, 1027, 0x1000, 0x1380); // Target beyond PT_LOAD.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    packed_fixture(bytes, 1027, 16, 0x1380);
    add_dynamic(bytes + 0x180, 4, 0, 0); // Missing paired size tag.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    put64(bytes + 0x248, 257); // R_AARCH64_ABS64 needs an external symbol.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);
    assert(stage.image == NULL);

    fixture(bytes);
    put64(bytes + 0x240, 0x5000); // Relocation destination outside image.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    put64(bytes + 64 + 40, ENGINE_STAGE_MAX_BYTES + 0x1000);
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_TOO_LARGE);

    fixture(bytes);
    put64(bytes + 0x180 + 8 * 16, 0); // Missing DT_RELRENT.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    put64(bytes + 0x2a0, 0x5000); // RELR target outside image.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    bytes[4] = 1; // ELFCLASS32
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    add_dynamic(bytes + 0x180, 2, 2, 24); // DT_PLTRELSZ
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);

    fixture(bytes);
    add_dynamic(bytes + 0x180, 2, 0x60000011, 0x12b0); // Unpaired Android packed RELA
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    add_dynamic(bytes + 0x180, 2, 17, 0x12b0); // DT_REL needs a different relocator.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);

    fixture(bytes);
    add_dynamic(bytes + 0x180, 2, 0x6000000f, 0x12b0); // Android packed REL
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);

    fixture(bytes);
    put64(bytes + 0x240, 0x1341); // Unaligned pointer relocation.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    put16(bytes + 56, 3);
    unsigned char *extra = bytes + 64 + 2 * 56;
    put32(extra, 1); // A second PT_LOAD leaves a gap at virtual address 0x2000.
    put64(extra + 16, 0x3000);
    put64(extra + 40, 0x100);
    put64(extra + 48, 1);
    put64(bytes + 0x240, 0x2000); // Must not relocate into the unmapped hole.
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_INVALID);

    fixture(bytes);
    add_dynamic(bytes + 0x180, 6, 0x6fffe000, 0x12a0); // Legacy Android RELR
    add_dynamic(bytes + 0x180, 7, 0x6fffe001, 16);
    add_dynamic(bytes + 0x180, 8, 0, 0); // No entry-size tag on legacy Android.
    assert(stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.relr_applied == 3);
    engine_stage_release(&stage);

    const uint64_t address = 0x1234567800ull;
    symbol_fixture(bytes, 1025, false); // GLOB_DAT from regular RELA.
    assert(resolve_fixture(bytes, &stage, fake_resolver, (void *)&address));
    assert(stage.symbol_applied == 1);
    assert(stage.relr_applied == 0);
    assert(get64(stage.image + 0x340) == address + 8);
    engine_stage_release(&stage);

    symbol_fixture(bytes, 1026, true); // JUMP_SLOT from DT_JMPREL.
    assert(resolve_fixture(bytes, &stage, fake_resolver, (void *)&address));
    assert(stage.symbol_applied == 1);
    assert(get64(stage.image + 0x340) == address + 8);
    engine_stage_release(&stage);

    symbol_fixture(bytes, 257, false); // ABS64 also uses S + A.
    assert(resolve_fixture(bytes, &stage, fake_resolver, (void *)&address));
    assert(get64(stage.image + 0x340) == address + 8);
    engine_stage_release(&stage);

    symbol_fixture(bytes, 1025, false);
    assert(!stage_fixture(bytes, sizeof(bytes), &stage));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);

    symbol_fixture(bytes, 1025, false);
    assert(!resolve_fixture(bytes, &stage, fake_resolver, NULL));
    assert(stage.status == ENGINE_STAGE_UNRESOLVED_SYMBOL);
    assert(strstr(stage.detail, "sample_symbol"));

    symbol_fixture(bytes, 1025, false);
    const uint64_t no_address = 0;
    assert(!resolve_fixture(bytes, &stage, fake_resolver, (void *)&no_address));
    assert(stage.status == ENGINE_STAGE_UNRESOLVED_SYMBOL);

    symbol_fixture(bytes, 1025, false);
    put32(bytes + 0x2d8, 24); // Out-of-range string-table offset.
    assert(!resolve_fixture(bytes, &stage, fake_resolver, (void *)&address));
    assert(stage.status == ENGINE_STAGE_INVALID);

    symbol_fixture(bytes, 1025, false);
    add_dynamic(bytes + 0x180, 7, 0x6ffffff0, 0x1320); // Versioned symbol table.
    add_dynamic(bytes + 0x180, 8, 0, 0);
    assert(!resolve_fixture(bytes, &stage, fake_resolver, (void *)&address));
    assert(stage.status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);

    puts("engine_stage_test: 35 checks passed");
    return 0;
}
