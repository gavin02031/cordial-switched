#ifndef MORROW_ANDROID_MAPPING_COMPAT_H
#define MORROW_ANDROID_MAPPING_COMPAT_H
#include <stdbool.h>
#include <stdint.h>
bool android_mapping_compat_resolve(const char *name, const char *version,
                                    const char *library, uint64_t *address);

#ifdef __SWITCH__
// The most recent svcSetMemoryPermission result from a guest protection
// change, so diagnostics can report the kernel's answer rather than errno.
uint32_t android_mapping_compat_last_kernel_result(void);
#endif
#endif
