#include "engine_module_registry.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static EngineModule modules[ENGINE_MODULE_REGISTRY_MAX];
static unsigned module_count;
static uint64_t registrations;

bool engine_module_register(uint64_t load_bias, const void *program_headers,
                            uint16_t program_header_count, const char *name)
{
    if (!program_headers || !program_header_count || !name ||
        strlen(name) >= ENGINE_MODULE_NAME_BYTES)
        return false;
    pthread_mutex_lock(&registry_lock);
    bool ok = module_count < ENGINE_MODULE_REGISTRY_MAX;
    if (ok) {
        EngineModule *slot = &modules[module_count++];
        slot->load_bias = load_bias;
        slot->program_headers = program_headers;
        slot->program_header_count = program_header_count;
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        registrations++;
    }
    pthread_mutex_unlock(&registry_lock);
    return ok;
}

unsigned engine_module_snapshot(EngineModule *out, unsigned capacity)
{
    if (!out || !capacity) return 0;
    pthread_mutex_lock(&registry_lock);
    unsigned count = module_count < capacity ? module_count : capacity;
    memcpy(out, modules, count * sizeof(*out));
    pthread_mutex_unlock(&registry_lock);
    return count;
}

uint64_t engine_module_adds(void)
{
    pthread_mutex_lock(&registry_lock);
    uint64_t count = registrations;
    pthread_mutex_unlock(&registry_lock);
    return count;
}

uint64_t engine_module_subs(void)
{
    return 0;
}

void engine_module_registry_reset(void)
{
    pthread_mutex_lock(&registry_lock);
    module_count = 0;
    registrations = 0;
    memset(modules, 0, sizeof(modules));
    pthread_mutex_unlock(&registry_lock);
}
