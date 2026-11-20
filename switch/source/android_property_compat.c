#include "android_property_compat.h"

#include <string.h>

// Intentionally EMPTY: no console run has observed the guest's property
// queries yet. The first run's system_property_read log lines decide which
// names appear here, each with a value whose provenance comment names its
// source. See switch/docs/presented-properties.md.
const AndroidPresentedProperty android_presented_properties[] = {
    // { "ro.example.name", "presented value", "why this value is true enough" },
};
const size_t android_presented_properties_count =
    sizeof(android_presented_properties) / sizeof(android_presented_properties[0]);

const AndroidPresentedProperty *android_property_find(
    const AndroidPresentedProperty *entries, size_t count, const char *name)
{
    if (!name || !*name)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].name && !strcmp(entries[i].name, name))
            return &entries[i];
    }
    return NULL;
}
