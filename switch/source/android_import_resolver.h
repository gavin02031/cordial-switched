#ifndef MORROW_ANDROID_IMPORT_RESOLVER_H
#define MORROW_ANDROID_IMPORT_RESOLVER_H
#include <stdbool.h>
#include <stdint.h>
bool android_import_resolve(const char *name, const char *version,
                            const char *library, uint64_t *address);
#endif
