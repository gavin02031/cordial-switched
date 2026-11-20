#include "android_log_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int calls;
    int priority;
    char tag[48];
    char message[256];
} Capture;

static void capture_message(int priority, const char *tag, const char *message,
                            void *context)
{
    Capture *capture = context;
    capture->calls++;
    capture->priority = priority;
    snprintf(capture->tag, sizeof(capture->tag), "%.47s", tag);
    snprintf(capture->message, sizeof(capture->message), "%.255s", message);
}

int main(void)
{
    Capture capture = {0};
    android_log_compat_set_sink(capture_message, &capture);

    assert(cordial_android_log_write(4, "startup", "initialising") == 1);
    assert(capture.calls == 1 && capture.priority == 4);
    assert(strcmp(capture.tag, "startup") == 0);
    assert(strcmp(capture.message, "initialising") == 0);

    assert(cordial_android_log_buf_write(2, 5, "warning", "unsupported audio") == 1);
    assert(capture.calls == 2 && capture.priority == 5);
    assert(strcmp(capture.message, "unsupported audio") == 0);

    assert(cordial_android_log_print(3, "runtime", "version %d: %s", 10, "ready") == 1);
    assert(capture.calls == 3 && capture.priority == 3);
    assert(strcmp(capture.message, "version 10: ready") == 0);

    assert(cordial_android_log_write(6, NULL, NULL) == 1);
    assert(strcmp(capture.tag, "(no tag)") == 0);
    assert(strcmp(capture.message, "(null)") == 0);

    const char *names[] = {"__android_log_write", "__android_log_buf_write",
                           "__android_log_print", "__android_log_assert"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint64_t address = 0;
        assert(android_log_compat_has(names[i]));
        assert(android_log_compat_resolve(names[i], &address, NULL));
        assert(address != 0);
    }

    uint64_t address = 123;
    assert(!android_log_compat_resolve("ALooper_prepare", &address, NULL));
    assert(address == 0); // Missing APIs must remain unresolved.
    assert(!android_log_compat_has("nonexistent_android_success_stub"));

    assert(!android_log_compat_resolve(NULL, &address, NULL));
    assert(!android_log_compat_resolve("__android_log_write", NULL, NULL));
    android_log_compat_set_sink(NULL, NULL);

    puts("android_log_compat_test: 7 checks passed");
    return 0;
}
