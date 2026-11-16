#define _POSIX_C_SOURCE 200809L
#ifdef __SWITCH__
#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine_exception_format.h"

// libnx normally provides a 1 KiB exception stack. A larger aligned stack
// gives the diagnostic formatter and best-effort filesystem path more room.
__attribute__((aligned(16))) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

// This handler is NOT a Linux SVC emulator. Horizon may dispatch faults in
// dynamically mapped code to kernel crash handling before userland receives
// them. Filesystem operations may themselves be unavailable in this context.
void __libnx_exception_handler(ThreadExceptionDump *context)
{
    static volatile int handling;
    if (!context || handling) return;
    handling = 1;

    char message[256];
    if (!engine_exception_format(message, sizeof(message), context->error_desc,
                                 context->pc.x, context->cpu_gprs[8].x,
                                 context->esr, context->far.x))
        return;

    // Debugger-visible fallback even when FAT/SD services are unavailable.
    svcOutputDebugString(message, strlen(message));

    // Only overwrite this one dedicated diagnostic. Never claim that a
    // missing file proves no trap occurred; exceptions may bypass userland.
    FILE *file = fopen("sdmc:/switch/cordial-switched/exception-last.txt", "w");
    if (!file) return;
    const size_t size = strlen(message);
    if (fwrite(message, 1, size, file) == size && fflush(file) == 0)
        fsync(fileno(file));
    fclose(file);
}
#endif
