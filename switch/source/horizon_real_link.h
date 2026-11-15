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

// L3 performs strict final-base relocation diagnostics. ZL+L3 requests an
// experimental JNI_OnLoad call. After that returns successfully, ZR+L3
// separately requests AGDK GameActivity.initializeNativeCode. Both operations
// can crash; a successful return does not imply rendering or playable login.
// ZL+ZR+L3 separately requests the four NativeGLInterface App Bridge V2
// JNI exports with typed but incomplete startup params after successful
// JNI_OnLoad. It can crash, and it does not prove login or gameplay.
// action: 0=diagnostic, 1=JNI_OnLoad, 2=GameActivity, 3=App Bridge V2.
void horizon_real_link_run(RealLinkResult *result, unsigned action);

#endif
