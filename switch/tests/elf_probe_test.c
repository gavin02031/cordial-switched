#include "elf_probe.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { IMAGE_BYTES = 1024 };

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value)
{
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static void put64(unsigned char *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static void make_valid_elf(unsigned char *bytes)
{
    memset(bytes, 0, IMAGE_BYTES);
    memcpy(bytes, "\x7f" "ELF", 4);
    bytes[4] = 2;  // ELFCLASS64
    bytes[5] = 1;  // Little endian
    bytes[6] = 1;  // Current ELF version
    put16(bytes + 16, 3);   // ET_DYN
    put16(bytes + 18, 183); // EM_AARCH64
    put32(bytes + 20, 1);
    put64(bytes + 32, 64);  // Program headers begin after ELF header
    put16(bytes + 52, 64);
    put16(bytes + 54, 56);
    put16(bytes + 56, 2);

    unsigned char *load = bytes + 64;
    put32(load, 1);         // PT_LOAD
    put64(load + 8, 0);
    put64(load + 16, 0x1000);
    put64(load + 32, IMAGE_BYTES);
    put64(load + 40, IMAGE_BYTES + 0x100);
    put64(load + 48, 0x1000);

    unsigned char *dyn = bytes + 64 + 56;
    put32(dyn, 2);          // PT_DYNAMIC
    put64(dyn + 8, 0x180);
    put64(dyn + 16, 0x1180);
    put64(dyn + 32, 5 * 16);
    put64(dyn + 40, 5 * 16);
    put64(dyn + 48, 8);

    unsigned char *entries = bytes + 0x180;
    put64(entries, 5);      // DT_STRTAB
    put64(entries + 8, 0x1300);
    put64(entries + 16, 10); // DT_STRSZ
    put64(entries + 24, 32);
    put64(entries + 32, 1);  // DT_NEEDED
    put64(entries + 40, 1);
    put64(entries + 48, 1);
    put64(entries + 56, 11);
    memcpy(bytes + 0x300, "\0liblog.so\0libc.so\0", 20);
}

static EngineProbeResult inspect(const unsigned char *bytes, size_t size)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(bytes, 1, size, file) == size);
    EngineProbeResult result;
    engine_probe_stream(file, &result);
    fclose(file);
    return result;
}

int main(void)
{
    unsigned char bytes[IMAGE_BYTES];
    make_valid_elf(bytes);

    EngineProbeResult result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_INSPECTED);
    assert(result.file_bytes == IMAGE_BYTES);
    assert(result.max_segment_alignment == 0x1000);
    assert(result.load_segments == 1);
    assert(result.needed_count == 2);
    assert(strcmp(result.needed[0], "liblog.so") == 0);
    assert(strcmp(result.needed[1], "libc.so") == 0);

    result = inspect(bytes, 40);
    assert(result.status == ENGINE_PROBE_INVALID_ELF);

    make_valid_elf(bytes);
    put16(bytes + 18, 62); // EM_X86_64 must not be treated as native ARM64.
    result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_WRONG_ARCH);

    make_valid_elf(bytes);
    bytes[4] = 1; // ELFCLASS32
    result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_INVALID_ELF);

    make_valid_elf(bytes);
    put64(bytes + 64 + 32, IMAGE_BYTES + 1); // PT_LOAD past EOF
    result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_INVALID_ELF);

    make_valid_elf(bytes);
    put64(bytes + 0x180 + 40, 99); // DT_NEEDED out of range
    result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_INVALID_ELF);

    make_valid_elf(bytes);
    put64(bytes + 64 + 48, 0x1800); // Non-power-of-two alignment
    result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_INVALID_ELF);

    make_valid_elf(bytes);
    put64(bytes + 0x180 + 64, 1); // No DT_NULL terminator
    result = inspect(bytes, sizeof(bytes));
    assert(result.status == ENGINE_PROBE_INVALID_ELF);

    engine_probe_file("/a/file/that/does/not/exist", &result);
    assert(result.status == ENGINE_PROBE_NOT_FOUND);

    puts("elf_probe_test: 9 checks passed");
    return 0;
}
