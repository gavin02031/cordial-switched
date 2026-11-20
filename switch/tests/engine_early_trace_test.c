#define _POSIX_C_SOURCE 200809L
#include "engine_early_trace.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/morrow-early-trace-XXXXXX";
    assert(mkdtemp(dir));
    assert(engine_early_trace_append(dir, "ctor_enter", 3, 0x1b2fe48, 0));
    assert(engine_early_trace_append(dir, "arena_mmap_enter", 0x7b2000, 0, 0));
    char path[192];
    assert(snprintf(path, sizeof(path), "%s/constructor-watchdog.txt", dir) > 0);
    FILE *file = fopen(path, "rb");
    assert(file);
    char line[256];
    assert(fgets(line, sizeof(line), file));
    assert(strstr(line, "trace_event=ctor_enter a=0x3 b=0x1b2fe48 c=0x0"));
    assert(fgets(line, sizeof(line), file));
    assert(strstr(line, "trace_event=arena_mmap_enter a=0x7b2000 b=0x0 c=0x0"));
    assert(!fgets(line, sizeof(line), file));
    assert(fclose(file) == 0);
    assert(!engine_early_trace_append(NULL, "ctor_enter", 0, 0, 0));
    assert(!engine_early_trace_append(dir, "", 0, 0, 0));
    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0);
    puts("durable early constructor and mmap breadcrumbs passed");
}
