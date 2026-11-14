#ifndef CORDIAL_HORIZON_REAL_LINK_H
#define CORDIAL_HORIZON_REAL_LINK_H

#include <stdbool.h>

typedef struct {
    bool attempted;
    bool log_saved;
    bool stage_prepared;
    char log_path[112];
    char detail[112];
} RealLinkResult;

// A strict, opt-in final-base RELOCATION preflight of the user-supplied
// Android ARM64 ELF. Never maps it RX or executes any Roblox instruction.
void horizon_real_link_run(RealLinkResult *result);

#endif
