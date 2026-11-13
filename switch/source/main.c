#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <switch.h>

#include "ui.h"
#include "engine_boot_log.h"
#include "horizon_loader_smoke.h"
#include "horizon_real_link.h"

// Keep the existing stage-XX naming while recording a fuller preflight trace.
static FILE *open_stage_log(char *path, size_t length)
{
    path[0] = 0;
    for (unsigned number = 1; number <= 99; number++) {
        char candidate[112];
        const int written = snprintf(candidate, sizeof(candidate),
                                     ENGINE_REPORT_DIR "/stage-%02u.txt", number);
        if (written <= 0 || (size_t)written >= sizeof(candidate))
            return NULL;
        FILE *existing = fopen(candidate, "rb");
        if (existing) {
            fclose(existing);
            continue;
        }
        if (errno != ENOENT)
            return NULL;
        FILE *trace = fopen(candidate, "wb");
        if (!trace)
            return NULL;
        snprintf(path, length, "%s", candidate);
        return trace;
    }
    return NULL;
}

static void try_engine_stage(UiState *state)
{
    state->stage_attempted = true;
    state->boot_log_attempted = true;
    state->boot_log_saved = false;
    FILE *trace = open_stage_log(state->boot_log_path,
                                 sizeof(state->boot_log_path));
    FILE *engine = fopen(ENGINE_LIBRARY_PATH, "rb");
    EngineBootReport result;
    engine_boot_preflight(engine, trace, &result);
    if (engine)
        fclose(engine);
    state->stage_status = result.stage_attempted ? result.stage_status
                                                : (engine ? ENGINE_STAGE_INVALID : ENGINE_STAGE_IO_ERROR);
    snprintf(state->stage_detail, sizeof(state->stage_detail), "%s", result.detail);
    if (trace) {
        const bool written = !ferror(trace);
        state->boot_log_saved = written && fclose(trace) == 0;
        if (!written)
            fclose(trace);
    }
}

static int failure_screen(const char *reason)
{
    // A graphics failure must remain legible on hardware without a debugger.
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("Cordial Switched could not start its graphics.\n\n%s\n\n", reason);
    printf("Please include this message when reporting a problem.\n");
    printf("Press PLUS to return to Homebrew Menu.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 1;
}

static void page_forward(UiState *state)
{
    state->page = (UiPage)((state->page + 1) % UI_PAGE_COUNT);
}

static void page_back(UiState *state)
{
    state->page = (UiPage)((state->page + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT);
}

static void handle_touch(UiState *state, unsigned x, unsigned y)
{
    // libnx touch coordinates are in the Switch's native 1280x720 space.
    if (x < 273) {
        if (y >= 229 && y < 289)
            state->page = UI_OVERVIEW;
        else if (y >= 298 && y < 358)
            state->page = UI_PERFORMANCE;
        else if (y >= 367 && y < 427)
            state->page = UI_ROADMAP;
        else if (y >= 436 && y < 496)
            state->page = UI_ENGINE;
        else if (y >= 505 && y < 565)
            state->page = UI_LOADER;
    } else if (state->page == UI_PERFORMANCE &&
               x >= 731 && x < 1176 && y >= 492 && y < 586) {
        state->animations = !state->animations;
    } else if (state->page == UI_ENGINE &&
               x >= 982 && x < 1186 && y >= 568 && y < 602) {
        jit_probe_run(&state->jit);
    } else if (state->page == UI_LOADER &&
               x >= 976 && x < 1188 && y >= 168 && y < 202) {
        state->loader_view = (state->loader_view + 1) % 3;
    }
}

int main(void)
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        return failure_screen("SDL could not initialise the video service.");

    // 720p is the Lite display's native resolution. No 1080p framebuffer cost.
    SDL_Window *window = SDL_CreateWindow("Cordial Switched", 0, 0,
                                           UI_WIDTH, UI_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Quit();
        return failure_screen("SDL could not create a 1280x720 window.");
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return failure_screen("No usable SDL rendering backend was found.");
    }

    SDL_RenderSetLogicalSize(renderer, UI_WIDTH, UI_HEIGHT);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_RendererInfo info = {0};
    SDL_GetRendererInfo(renderer, &info);

    if (!ui_init(renderer)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return failure_screen("The Switch shared font or UI resources failed to load.");
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    UiState state = {
        .page = UI_OVERVIEW,
        .animations = true,
        .accelerated = (info.flags & SDL_RENDERER_ACCELERATED) != 0,
        .vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0,
        .fps = 0.0,
        .frame_ms = 0.0,
    };
    snprintf(state.renderer_name, sizeof(state.renderer_name), "%s",
             info.name ? info.name : "unknown");
    engine_probe_file(ENGINE_LIBRARY_PATH, &state.engine);

    bool quit = false;
    bool was_touching = false;
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 sample_start = SDL_GetPerformanceCounter();
    Uint64 sample_frames = 0;
    double sample_frame_ms = 0.0;

    while (!quit && appletMainLoop()) {
        const Uint64 frame_start = SDL_GetPerformanceCounter();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                quit = true;
        }

        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad);
        if (pressed & HidNpadButton_Plus)
            break;
        if (pressed & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_R))
            page_forward(&state);
        if (pressed & (HidNpadButton_B | HidNpadButton_Left | HidNpadButton_L))
            page_back(&state);
        if (state.page == UI_PERFORMANCE && (pressed & HidNpadButton_X))
            state.animations = !state.animations;
        if ((state.page == UI_ENGINE || state.page == UI_LOADER) &&
            (pressed & HidNpadButton_Y)) {
            engine_probe_file(ENGINE_LIBRARY_PATH, &state.engine);
            state.report_attempted = false;
            state.stage_attempted = false;
            state.boot_log_attempted = false;
        }
        if (state.page == UI_ENGINE && (pressed & HidNpadButton_X))
            jit_probe_run(&state.jit);
        if (state.page == UI_LOADER && (pressed & HidNpadButton_X))
            state.loader_view = (state.loader_view + 1) % 3;
        if (state.page == UI_LOADER && (pressed & HidNpadButton_ZL)) {
            state.report_attempted = true;
            state.report_saved = engine_probe_export_imports(
                ENGINE_LIBRARY_PATH, state.report_path, sizeof(state.report_path));
        }
        if (state.page == UI_LOADER && (pressed & HidNpadButton_ZR))
            try_engine_stage(&state);
        if (state.page == UI_LOADER && (pressed & HidNpadButton_StickR))
            horizon_loader_smoke_run(&state.loader_smoke);
        if (state.page == UI_LOADER && (pressed & HidNpadButton_StickL))
            horizon_real_link_run(&state.real_link);

        HidTouchScreenState touch = {0};
        const bool touching = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
        if (touching && !was_touching)
            handle_touch(&state, touch.touches[0].x, touch.touches[0].y);
        was_touching = touching;

        ui_draw(renderer, &state, (double)SDL_GetTicks() / 1000.0);
        SDL_RenderPresent(renderer);

        // Only manually pace a driver which cannot provide a 60 Hz swap.
        if (!state.vsync) {
            const Uint64 deadline = frame_start + frequency / 60;
            const double elapsed_ms = (double)(SDL_GetPerformanceCounter() - frame_start)
                * 1000.0 / (double)frequency;
            if (elapsed_ms < 15.0)
                SDL_Delay((Uint32)(15.0 - elapsed_ms));
            while (SDL_GetPerformanceCounter() < deadline)
                SDL_Delay(0);
        }

        const Uint64 frame_end = SDL_GetPerformanceCounter();
        sample_frame_ms += (double)(frame_end - frame_start) * 1000.0 / (double)frequency;
        sample_frames++;

        const double window_seconds = (double)(frame_end - sample_start) / (double)frequency;
        if (window_seconds >= 1.0) {
            state.fps = (double)sample_frames / window_seconds;
            state.frame_ms = sample_frame_ms / (double)sample_frames;
            sample_start = frame_end;
            sample_frames = 0;
            sample_frame_ms = 0.0;
        }
    }

    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
