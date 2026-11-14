#include "android_log_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AndroidLogSink log_sink;
static void *log_context;

void android_log_compat_set_sink(AndroidLogSink sink, void *context)
{
    log_sink = sink;
    log_context = context;
}

static char priority_letter(int priority)
{
    switch (priority) {
    case 2: return 'V';
    case 3: return 'D';
    case 4: return 'I';
    case 5: return 'W';
    case 6: return 'E';
    case 7: return 'F';
    default: return '?';
    }
}

static int log_message(int priority, const char *tag, const char *message)
{
    const char *safe_tag = tag ? tag : "(no tag)";
    const char *safe_message = message ? message : "(null)";
    if (log_sink) {
        log_sink(priority, safe_tag, safe_message, log_context);
        return 1;
    }
    // stderr is a real fallback, not a pretend Android log buffer. A future
    // Horizon engine runner will install a durable log sink before startup.
    if (fprintf(stderr, "%c/%s: %s\n", priority_letter(priority),
                safe_tag, safe_message) < 0)
        return -EIO;
    return 1;
}

int cordial_android_log_write(int priority, const char *tag, const char *message)
{
    return log_message(priority, tag, message);
}

int cordial_android_log_buf_write(int buffer_id, int priority,
                                   const char *tag, const char *message)
{
    (void)buffer_id;
    // Horizon does not expose Android's separate main/radio/system buffers.
    return log_message(priority, tag, message);
}

int cordial_android_log_print(int priority, const char *tag,
                              const char *format, ...)
{
    if (!format)
        return -EINVAL;
    char message[4096];
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0)
        return -EINVAL;
    return log_message(priority, tag, message);
}

_Noreturn void cordial_android_log_assert(const char *condition,
                                          const char *tag,
                                          const char *format, ...)
{
    char message[4096];
    if (format) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);
    } else {
        snprintf(message, sizeof(message), "Assertion failed: %.128s",
                 condition ? condition : "(unknown)");
    }
    log_message(7, tag, message);
    abort(); // Android's fatal assertion must not return as if it succeeded.
}

bool android_log_compat_resolve(const char *name, uint64_t *address, void *context)
{
    (void)context;
    if (!name || !address)
        return false;
    *address = 0;
    if (strcmp(name, "__android_log_write") == 0)
        *address = (uint64_t)(uintptr_t)&cordial_android_log_write;
    else if (strcmp(name, "__android_log_buf_write") == 0)
        *address = (uint64_t)(uintptr_t)&cordial_android_log_buf_write;
    else if (strcmp(name, "__android_log_print") == 0)
        *address = (uint64_t)(uintptr_t)&cordial_android_log_print;
    else if (strcmp(name, "__android_log_assert") == 0)
        *address = (uint64_t)(uintptr_t)&cordial_android_log_assert;
    return *address != 0;
}

bool android_log_compat_has(const char *name)
{
    uint64_t address = 0;
    return android_log_compat_resolve(name, &address, NULL);
}
