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

// L3 performs strict final-base relocation diagnostics. Holding ZL with L3
// additionally requests an experimental JNI_OnLoad call, only if *all* live
// mapping, JNI, constructors and TLS preconditions have been verified.
// The engine may still crash; a successful return does not imply gameplay.
void horizon_real_link_run(RealLinkResult *result, bool request_execution);

#endif
