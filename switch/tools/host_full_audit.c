// Host pre-console gate: run the real guest ELF through the same
// engine_stage_stream_audit_at machinery the console's full_audit uses, with
// the same android_import_resolve chain. Nothing is executed - the scratch
// image stays NX and is discarded. This exercises the relocation-application
// path (RELA/RELR/packed tables, addends, version+library ABI keys, segment
// planning, dyninit section verification) over the actual binary, which the
// Python import audit does not touch.
//
// Expected on a 64-bit Linux host for libroblox.so: status
// UNRESOLVED_SYMBOL (the host build compiles out the 91-name EGL/GLES2
// catalogue and the AArch64-only jump bridge), relocation scan complete,
// zero unsupported relocations, and a required-missing set that is exactly
// the 93 target-registered names. Any other outcome is a pre-console
// finding. The guest ELF is proprietary: pass its path, never commit it.
//
// Build (from the repo root):
//   PROVIDERS=$(grep -v '^#' switch/source/PROVIDER_SOURCES.txt | sed 's|^|switch/source/|')
//   cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source
//     switch/tools/host_full_audit.c switch/source/engine_stage.c
//     switch/source/engine_boot_log.c switch/source/elf_probe.c
//     $PROVIDERS -lm -pthread -o /tmp/morrow-host-full-audit
//   /tmp/morrow-host-full-audit /path/to/libroblox.so

#include "engine_stage.h"
#include "android_import_resolver.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Distinct (name, version, library) keys the resolver could not bind. The
// audit continues past missing imports, so this set is complete by the end.
#define MAX_MISSING 1024
static char missing_keys[MAX_MISSING][176];
static unsigned missing_count;

static void record_missing(const char *name, const char *version,
                           const char *library)
{
    char key[176];
    snprintf(key, sizeof(key), "%s @ %s from %s", name,
             version ? version : "unversioned",
             library ? library : "unspecified");
    for (unsigned i = 0; i < missing_count; ++i)
        if (strcmp(missing_keys[i], key) == 0) return;
    if (missing_count < MAX_MISSING)
        memcpy(missing_keys[missing_count++], key, strlen(key) + 1);
}

static int compare_keys(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static bool resolve(const char *name, const char *version,
                    const char *library, uint64_t *address, void *context)
{
    (void)context;
    bool found = android_import_resolve(name, version, library, address);
    if (!found && name) record_missing(name, version, library);
    return found;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <guest-elf>\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 2;
    }
    EngineStage stage = {0};
    // The base only shapes audited addresses; the NX scratch is discarded.
    bool complete = engine_stage_stream_audit_at(file, &stage, resolve, NULL,
                                                 UINT64_C(0x100000000));
    fclose(file);

    printf("status=%d\n", (int)stage.status);
    printf("scan_returned=%s\n", complete ? "true" : "false");
    printf("relocation_scan_complete=%s\n",
           stage.relocation_scan_complete ? "true" : "false");
    printf("rela_applied=%u\n", stage.rela_applied);
    printf("relr_applied=%u\n", stage.relr_applied);
    printf("packed_applied=%u\n", stage.packed_applied);
    printf("symbol_applied=%u\n", stage.symbol_applied);
    printf("weak_undefined_relocations=%u\n", stage.weak_undefined_relocations);
    printf("required_missing_relocations=%u\n", stage.required_missing_relocations);
    printf("unsupported_relocations_seen=%u\n", stage.unsupported_relocations_seen);
    printf("tls_relocations_seen=%u\n", stage.tls_relocations_seen);
    printf("tlsdesc_relocations_seen=%u\n", stage.tlsdesc_relocations_seen);
    printf("dyninit_status=%s\n", engine_dyninit_status_name(stage.dyninit_status));
    printf("first_required_missing=%s @ %s from %s\n",
           stage.first_required_name, stage.first_required_version,
           stage.first_required_library);
    printf("detail=%s\n", stage.detail);
    qsort(missing_keys, missing_count, sizeof(missing_keys[0]), compare_keys);
    printf("missing_distinct=%u\n", missing_count);
    for (unsigned i = 0; i < missing_count; ++i)
        printf("missing: %s\n", missing_keys[i]);
    engine_stage_release(&stage);
    return 0;
}
