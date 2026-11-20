#ifndef CORDIAL_SWITCH_ANDROID_LOG_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_LOG_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// A real liblog implementation for the four unversioned Android imports seen
// in the user's PLT report. No code from libroblox.so is run by this module.
typedef void (*AndroidLogSink)(int priority, const char *tag,
                               const char *message, void *context);

void android_log_compat_set_sink(AndroidLogSink sink, void *context);
int cordial_android_log_write(int priority, const char *tag, const char *message);
int cordial_android_log_buf_write(int buffer_id, int priority,
                                   const char *tag, const char *message);
int cordial_android_log_print(int priority, const char *tag,
                              const char *format, ...);
_Noreturn void cordial_android_log_assert(const char *condition,
                                          const char *tag,
                                          const char *format, ...);

// Satisfy only names for which there is an actual implementation. This is a
// host-tested callback for the non-executable scratch linker, not a dynamic
// linker capable of starting the Android engine.
bool android_log_compat_resolve(const char *name, uint64_t *address, void *context);
bool android_log_compat_has(const char *name);

#endif
