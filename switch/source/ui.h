#ifndef CORDIAL_SWITCH_UI_H
#define CORDIAL_SWITCH_UI_H

#include <stdbool.h>

#include <SDL.h>

#include "elf_probe.h"
#include "engine_stage.h"
#include "jit_probe.h"

#define UI_WIDTH 1280
#define UI_HEIGHT 720

typedef enum {
    UI_OVERVIEW = 0,
    UI_PERFORMANCE,
    UI_ROADMAP,
    UI_ENGINE,
    UI_LOADER,
    UI_PAGE_COUNT
} UiPage;

typedef struct {
    UiPage page;
    bool animations;
    unsigned loader_view; // Profile, required libraries, imported symbols.
    bool report_attempted;
    bool report_saved;
    char report_path[112];
    bool stage_attempted;
    EngineStageStatus stage_status;
    char stage_detail[120];
    bool boot_log_attempted;
    bool boot_log_saved;
    char boot_log_path[112];
    bool accelerated;
    bool vsync;
    double fps;
    double frame_ms;
    char renderer_name[48];
    EngineProbeResult engine;
    JitProbeResult jit;
} UiState;

bool ui_init(SDL_Renderer *renderer);
void ui_draw(SDL_Renderer *renderer, const UiState *state, double seconds);
void ui_shutdown(void);

#endif
