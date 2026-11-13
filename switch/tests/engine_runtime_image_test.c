#include "engine_runtime_image.h"
#include "engine_loader_fixture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { FIXTURE_BYTES = 1024 };

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

static void fixture(unsigned char *bytes)
{
    memset(bytes, 0, FIXTURE_BYTES);
    memcpy(bytes, "\x7f" "ELF", 4);
    bytes[4] = 2;
    bytes[5] = 1;
    bytes[6] = 1;
    put16(bytes + 16, 3);   // ET_DYN
    put16(bytes + 18, 183); // EM_AARCH64
    put32(bytes + 20, 1);
    put64(bytes + 32, 64);
    put16(bytes + 52, 64);
    put16(bytes + 54, 56);
    put16(bytes + 56, 2);

    unsigned char *load = bytes + 64;
    put32(load, 1); // PT_LOAD
    put32(load + 4, 5); // PF_R | PF_X
    put64(load + 8, 0);
    put64(load + 16, 0x1000);
    put64(load + 32, FIXTURE_BYTES);
    put64(load + 40, 0x600);
    put64(load + 48, 0x1000);

    unsigned char *dynamic = bytes + 64 + 56;
    put32(dynamic, 2); // PT_DYNAMIC
    put32(dynamic + 4, 4); // PF_R
    put64(dynamic + 8, 0x180);
    put64(dynamic + 16, 0x1180);
    put64(dynamic + 32, 6 * 16);
    put64(dynamic + 40, 6 * 16);
    put64(dynamic + 48, 8);

    put64(bytes + 0x180, 5);      // DT_STRTAB
    put64(bytes + 0x188, 0x1300);
    put64(bytes + 0x190, 10);     // DT_STRSZ
    put64(bytes + 0x198, 16);
    put64(bytes + 0x1a0, 7);      // DT_RELA
    put64(bytes + 0x1a8, 0x1240);
    put64(bytes + 0x1b0, 8);      // DT_RELASZ
    put64(bytes + 0x1b8, 24);
    put64(bytes + 0x1c0, 9);      // DT_RELAENT
    put64(bytes + 0x1c8, 24);
    put64(bytes + 0x240, 0x1340); // R_AARCH64_RELATIVE destination
    put64(bytes + 0x248, 1027);
    put64(bytes + 0x250, 0x1380); // Address-sensitive addend
}

static FILE *fixture_file(unsigned char *bytes)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(bytes, 1, FIXTURE_BYTES, file) == FIXTURE_BYTES);
    assert(fseek(file, 0, SEEK_SET) == 0);
    return file;
}

typedef struct {
    uint64_t chosen_base;
    uint64_t expected_addend;
    unsigned reserves;
    unsigned maps;
    unsigned releases;
    bool fail_reserve;
    bool fail_map;
    bool fail_release;
    bool relocated_correctly;
} FakeMemory;

static bool fake_reserve(void *context, const EngineMemoryPlan *plan, uint64_t *base)
{
    FakeMemory *fake = context;
    fake->reserves++;
    assert(plan->image_bytes == 4096);
    assert(plan->runs[0].permission == ENGINE_MEMORY_READ_EXECUTE);
    if (fake->fail_reserve) return false;
    *base = fake->chosen_base;
    return true;
}

static bool fake_map(void *context, const EngineMemoryPlan *plan, void *source)
{
    FakeMemory *fake = context;
    fake->maps++;
    assert(plan->image_bytes == 4096);
    assert(((uintptr_t)source & 4095u) == 0);
    fake->relocated_correctly =
        get64((unsigned char *)source + 0x340) == fake->chosen_base +
            (fake->expected_addend ? fake->expected_addend : 0x380);
    return !fake->fail_map;
}

static bool fake_release(void *context)
{
    FakeMemory *fake = context;
    fake->releases++;
    return !fake->fail_release;
}

static EngineImagePlatform fake_platform(FakeMemory *fake)
{
    return (EngineImagePlatform){fake, fake_reserve, fake_map, fake_release};
}

int main(void)
{
    unsigned char bytes[FIXTURE_BYTES];
    fixture(bytes);
    FakeMemory fake = {.chosen_base = 0x900000};
    EngineImagePlatform platform = fake_platform(&fake);
    EngineRuntimeImage image = {0};
    FILE *file = fixture_file(bytes);
    if (!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image)) {
        fprintf(stderr, "unexpected prepare failure: status=%d stage=%d %s\n",
                image.status, image.stage_status, image.detail);
        assert(false);
    }
    assert(image.status == ENGINE_IMAGE_MAPPED_NOT_EXECUTED);
    assert(image.stage_status == ENGINE_STAGE_PREPARED);
    assert(image.mapped && image.reserved && image.runtime_base == 0x900000);
    assert(fake.reserves == 1 && fake.maps == 1 && fake.releases == 0);
    assert(fake.relocated_correctly);
    assert(engine_runtime_image_release(&platform, &image));
    assert(!image.reserved && !image.aligned_source && !image.plan.runs);
    assert(fake.releases == 1);
    fclose(file);

    // The same tiny ELF used on the Lite must link against the final address.
    engine_loader_fixture_make(bytes);
    fake = (FakeMemory){.chosen_base = 0x900000, .expected_addend = 0x300};
    file = fixture_file(bytes);
    assert(engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(fake.relocated_correctly && image.mapped);
    assert(engine_runtime_image_release(&platform, &image));
    fclose(file);
    fixture(bytes);

    // A reservation error must not call release, map, or stage an image.
    fake = (FakeMemory){.chosen_base = 0x900000, .fail_reserve = true};
    file = fixture_file(bytes);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(image.status == ENGINE_IMAGE_RESERVE_FAILED);
    assert(fake.reserves == 1 && fake.maps == 0 && fake.releases == 0);
    fclose(file);

    // The linker fails closed for an unsupported Android relocation.
    put64(bytes + 0x248, 9999);
    fake = (FakeMemory){.chosen_base = 0x900000};
    file = fixture_file(bytes);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(image.status == ENGINE_IMAGE_STAGE_FAILED);
    assert(image.stage_status == ENGINE_STAGE_UNSUPPORTED_RELOCATION);
    assert(fake.reserves == 1 && fake.maps == 0 && fake.releases == 1);
    fclose(file);
    fixture(bytes);

    // An invalid future base must release the reservation before staging.
    fake = (FakeMemory){.chosen_base = 0x900001};
    file = fixture_file(bytes);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(image.status == ENGINE_IMAGE_RESERVE_FAILED);
    assert(fake.reserves == 1 && fake.maps == 0 && fake.releases == 1);
    fclose(file);

    // Failed maps have an explicit cleanup path. On a failed kernel rollback,
    // the aligned source must stay alive for a later unmap retry.
    fake = (FakeMemory){.chosen_base = 0x900000, .fail_map = true,
                        .fail_release = true};
    file = fixture_file(bytes);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(image.status == ENGINE_IMAGE_CLEANUP_FAILED);
    assert(image.reserved && image.aligned_source && image.plan.runs);
    assert(fake.reserves == 1 && fake.maps == 1 && fake.releases == 1);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(fake.reserves == 1);
    fake.fail_release = false;
    assert(engine_runtime_image_release(&platform, &image));
    assert(!image.reserved && !image.aligned_source && !image.plan.runs);
    assert(fake.releases == 2);
    fclose(file);

    fake = (FakeMemory){.chosen_base = 0x900000, .fail_map = true};
    file = fixture_file(bytes);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(image.status == ENGINE_IMAGE_MAP_FAILED);
    assert(fake.reserves == 1 && fake.maps == 1 && fake.releases == 1);
    fclose(file);

    bytes[18] = 62; // x86-64 must fail before reserving Switch code memory.
    fake = (FakeMemory){.chosen_base = 0x900000};
    file = fixture_file(bytes);
    assert(!engine_runtime_image_prepare(file, NULL, NULL, &platform, &image));
    assert(image.status == ENGINE_IMAGE_INVALID_ELF);
    assert(fake.reserves == 0 && fake.maps == 0 && fake.releases == 0);
    fclose(file);

    puts("engine_runtime_image_test: all assertions passed");
    return 0;
}
