#define _POSIX_C_SOURCE 200809L
#include "engine_module_registry.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    engine_module_registry_reset();
    EngineModule out[ENGINE_MODULE_REGISTRY_MAX];
    assert(engine_module_snapshot(out, ENGINE_MODULE_REGISTRY_MAX) == 0);
    assert(engine_module_adds() == 0);
    assert(engine_module_subs() == 0);

    static const unsigned char phdrs_a[56 * 12];
    assert(engine_module_register(0x100000, phdrs_a, 12, "libroblox.so"));
    assert(engine_module_snapshot(out, ENGINE_MODULE_REGISTRY_MAX) == 1);
    assert(out[0].load_bias == 0x100000);
    assert(out[0].program_headers == phdrs_a);
    assert(out[0].program_header_count == 12);
    assert(strcmp(out[0].name, "libroblox.so") == 0);
    assert(engine_module_adds() == 1);

    static const unsigned char phdrs_b[56 * 4];
    assert(engine_module_register(0x200000, phdrs_b, 4, "libengine2.so"));
    assert(engine_module_snapshot(out, ENGINE_MODULE_REGISTRY_MAX) == 2);
    assert(out[1].load_bias == 0x200000);

    // Invalid registrations change nothing.
    assert(!engine_module_register(0x300000, NULL, 4, "bad.so"));
    assert(!engine_module_register(0x300000, phdrs_b, 0, "bad.so"));
    assert(!engine_module_register(0x300000, phdrs_b, 4, NULL));
    char long_name[ENGINE_MODULE_NAME_BYTES + 8];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    assert(!engine_module_register(0x300000, phdrs_b, 4, long_name));
    assert(engine_module_snapshot(out, ENGINE_MODULE_REGISTRY_MAX) == 2);
    assert(engine_module_adds() == 2);

    // Fill the registry; the next register fails and keeps prior records.
    for (unsigned i = 2; i < ENGINE_MODULE_REGISTRY_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "fill%u.so", i);
        assert(engine_module_register(0x1000 * (uint64_t)i, phdrs_b, 1, name));
    }
    assert(!engine_module_register(0x900000, phdrs_b, 1, "overflow.so"));
    EngineModule capped[2];
    assert(engine_module_snapshot(capped, 2) == 2);

    engine_module_registry_reset();
    assert(engine_module_snapshot(out, ENGINE_MODULE_REGISTRY_MAX) == 0);
    assert(engine_module_adds() == 0);
    return 0;
}
