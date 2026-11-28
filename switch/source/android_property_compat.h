#ifndef CORDIAL_ANDROID_PROPERTY_COMPAT_H
#define CORDIAL_ANDROID_PROPERTY_COMPAT_H

#include <stdbool.h>
#include <stddef.h>

// Presented-property table for Bionic's __system_property_get.
//
// Values in this table are PRESENTED, not measured: they describe the device
// profile we choose to show the guest, the same way an emulator answers. An
// entry is added only after the console run log shows the guest asking for
// that name (system_property_read lines), and each entry must carry a comment
// with its value's source. Never invent a query the guest has not made, and
// never copy a value from a copyrighted dump. Bionic caps values at 92 bytes
// (PROP_VALUE_MAX); longer entries are a table bug, not data to truncate.

typedef struct {
    const char *name;
    const char *value;
    // Short provenance tag, e.g. "console-observed 2026-11-19; value from
    // AOSP default" or "guest query log; INFERRED from Roblox requirements".
    const char *provenance;
} AndroidPresentedProperty;

// Pure lookup core, separated from the production table so host tests can
// exercise hits, misses and duplicates with their own arrays.
const AndroidPresentedProperty *android_property_find(
    const AndroidPresentedProperty *entries, size_t count, const char *name);

// The production table. Empty until the first console run supplies observed
// property names; adding rows there must not require touching provider code.
extern const AndroidPresentedProperty android_presented_properties[];
extern const size_t android_presented_properties_count;

#endif
