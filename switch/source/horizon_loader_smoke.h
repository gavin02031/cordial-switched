#ifndef CORDIAL_HORIZON_LOADER_SMOKE_H
#define CORDIAL_HORIZON_LOADER_SMOKE_H

#include <stdbool.h>

typedef enum {
    LOADER_SMOKE_NOT_RUN = 0,
    LOADER_SMOKE_VERIFIED,
    LOADER_SMOKE_FAILED,
    LOADER_SMOKE_CLEANUP_FAILED
} LoaderSmokeStatus;

typedef struct {
    LoaderSmokeStatus status;
    bool log_saved;
    char log_path[112];
    char detail[112];
} LoaderSmokeResult;

// Explicit synthetic hardware check; never loads or runs libroblox.so.
void horizon_loader_smoke_run(LoaderSmokeResult *result);

#endif
