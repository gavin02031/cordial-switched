#ifndef CORDIAL_HORIZON_REAL_LINK_H
#define CORDIAL_HORIZON_REAL_LINK_H

#include <stdbool.h>
#include <stdio.h>

typedef struct {
    bool attempted;
    bool log_saved;
    bool stage_prepared;
    char log_path[112];
    char detail[112];
} RealLinkResult;

// L3 performs strict final-base relocation diagnostics. Native execution
// requires the UI renderer to be destroyed first: ZL+ZR+Minus on Loader
// initiates the experimental exclusive graphics handoff. These methods
// must not be called with SDL still owning the Horizon NWindow.
// JNI_OnLoad, GameActivity and the five App Bridge calls can crash. A
// successful return does not imply rendering, login or gameplay.
// Surface update is reserved for a real resize event after StartApp.
// The exclusive path optionally tries a single main-thread message callback
// only after the five entries returned. This is NOT a recurring event pump.
// action: 0=diagnostic, 1=JNI_OnLoad, 2=GameActivity, 3=App Bridge V2,
//         4=one-shot main-thread message callback.
void horizon_real_link_run(RealLinkResult *result, unsigned action);

// Called only after SDL_DestroyRenderer, SDL_DestroyWindow and SDL_Quit.
// The native engine and its RX mapping remain pinned until process exit.
void horizon_real_link_enable_exclusive_graphics(void);
bool horizon_real_link_jni_onload_ready(void);
bool horizon_real_link_app_bridge_ready(void);

// Opt-in, bounded main-thread message-pump experiment for the exclusive
// graphics path only. The caller holds a numbered link report open for
// append. Every call is synchronously journaled before entering guest code.
// Returns false on a missing prerequisite, SD logging failure, exception,
// or an entry that did not return. Max 120 calls per exclusive session.
bool horizon_real_link_pump_exclusive(FILE *trace, const char *report_path,
                                      unsigned call_number);

#endif
