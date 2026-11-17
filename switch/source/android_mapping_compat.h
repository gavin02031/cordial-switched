#ifndef MORROW_ANDROID_MAPPING_COMPAT_H
#define MORROW_ANDROID_MAPPING_COMPAT_H
#include <stdbool.h>
#include <stdint.h>
bool android_mapping_compat_resolve(const char *name, const char *version,
                                    const char *library, uint64_t *address);
#endif
