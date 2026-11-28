#include "engine_loader_fixture.h"

#include <stdint.h>
#include <string.h>

static void put16(unsigned char *p, uint16_t n) {
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}
static void put32(unsigned char *p, uint32_t n) {
    put16(p, (uint16_t)n);
    put16(p + 2, (uint16_t)(n >> 16));
}
static void put64(unsigned char *p, uint64_t n) {
    put32(p, (uint32_t)n);
    put32(p + 4, (uint32_t)(n >> 32));
}

void engine_loader_fixture_make(unsigned char bytes[ENGINE_LOADER_FIXTURE_BYTES])
{
    memset(bytes, 0, ENGINE_LOADER_FIXTURE_BYTES);
    memcpy(bytes, "\x7f" "ELF", 4);
    bytes[4] = 2; bytes[5] = 1; bytes[6] = 1;
    put16(bytes + 16, 3);   // ET_DYN
    put16(bytes + 18, 183); // EM_AARCH64
    put32(bytes + 20, 1);
    put64(bytes + 32, 64);
    put16(bytes + 52, 64); put16(bytes + 54, 56); put16(bytes + 56, 2);

    unsigned char *load = bytes + 64;
    put32(load, 1); put32(load + 4, 5); // PT_LOAD, PF_R|PF_X
    put64(load + 16, 0x1000);
    put64(load + 32, ENGINE_LOADER_FIXTURE_BYTES);
    put64(load + 40, 0x600); put64(load + 48, 0x1000);

    unsigned char *dynamic = bytes + 64 + 56;
    put32(dynamic, 2); put32(dynamic + 4, 4);
    put64(dynamic + 8, 0x180); put64(dynamic + 16, 0x1180);
    put64(dynamic + 32, 6 * 16); put64(dynamic + 40, 6 * 16);
    put64(dynamic + 48, 8);
    put64(bytes + 0x180, 5); put64(bytes + 0x188, 0x1380); // DT_STRTAB
    put64(bytes + 0x190, 10); put64(bytes + 0x198, 16);   // DT_STRSZ
    put64(bytes + 0x1a0, 7); put64(bytes + 0x1a8, 0x1240); // DT_RELA
    put64(bytes + 0x1b0, 8); put64(bytes + 0x1b8, 24); // DT_RELASZ
    put64(bytes + 0x1c0, 9); put64(bytes + 0x1c8, 24); // DT_RELAENT
    // Only original movz w0,#42; ret; its address is relocated below.
    put32(bytes + ENGINE_LOADER_FUNCTION_OFFSET, 0x52800540u);
    put32(bytes + ENGINE_LOADER_FUNCTION_OFFSET + 4, 0xD65F03C0u);
    put64(bytes + 0x240, 0x1340);
    put64(bytes + 0x248, 1027); // R_AARCH64_RELATIVE
    put64(bytes + 0x250, 0x1300);
}
