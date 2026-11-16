#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>
#include <switch.h>

#include "ui.h"
#include "ui_navigation.h"
#include "engine_boot_log.h"
#include "engine_run_journal.h"
#include "horizon_loader_smoke.h"
#include "horizon_real_link.h"
#include "morrow_gles_smoke.h"
#include "android_egl_gles_compat.h"
#ifdef MORROW_HAS_JNIVM
#include "morrow_jni_runtime.h"

static void bind_switch_system_locale(void)
{
    // libnx reads the real console setting; the packed language code is an
    // eight-byte ASCII tag, not an Android emulator locale or an IP guess.
    if (R_FAILED(setInitialize()))
        return;
    u64 language_code = 0;
    const Result status = setGetSystemLanguage(&language_code);
    setExit();
    if (R_FAILED(status))
        return;
    char language_tag[9];
    for (unsigned i = 0; i < 8; i++)
        language_tag[i] = (char)((language_code >> (8 * i)) & 0xff);
    language_tag[8] = '\0';
    morrow_jni_runtime_bind_system_locale(language_tag);
}

static void bind_switch_sd_free_space(void)
{
    // fsdev owns the already-mounted SD service. Do not open a second mount,
    // guess the card's capacity, or count internal NAND as game-cache space.
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd) return;
    s64 bytes = -1;
    if (R_SUCCEEDED(fsFsGetFreeSpace(sd, "/", &bytes)) && bytes >= 0)
        morrow_jni_runtime_bind_storage_available_bytes(bytes);
}

static bool prepare_jni_runtime(void)
{
    // libjnivm registers LocalStorageManager only while it creates the VM.
    // Starting it before reading the SD mount made the exclusive engine path
    // silently lose this class even when the storage service was available.
    bind_switch_sd_free_space();
    if (!morrow_jni_runtime_start())
        return false;
    bind_switch_system_locale();
    morrow_jni_runtime_game_activity();
    morrow_jni_runtime_local_storage_manager();
    return true;
}
#endif

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

#ifdef MORROW_HAS_JNIVM
static void exclusive_graphics_snapshot(FILE *trace, unsigned sample)
{
    if (!trace) return;
    AndroidGuestGraphicsStats graphics = {0};
    android_egl_gles_compat_read_guest_stats(&graphics);
    fprintf(trace,
            "exclusive_sample_%03u:surface=%llu make_current=%llu "
            "clear=%llu swap=%llu\n",
            sample,
            (unsigned long long)graphics.create_surface_succeeded,
            (unsigned long long)graphics.make_current_succeeded,
            (unsigned long long)graphics.clear_calls,
            (unsigned long long)graphics.swap_succeeded);
    fflush(trace);
    fsync(fileno(trace));
}

static int exclusive_engine_launch(void)
{
    // This function is reached ONLY after SDL has destroyed its renderer,
    // window and video state. The Horizon NWindow can now be owned by guest
    // EGL without SDL attempting concurrent presents on the same display.
    horizon_real_link_enable_exclusive_graphics();
    if (!prepare_jni_runtime())
        return failure_screen("Exclusive engine launch: real JNI VM unavailable.");
    morrow_jni_runtime_bind_horizon_surface(
        nwindowGetDefault(), UI_WIDTH, UI_HEIGHT);
    morrow_jni_runtime_bind_asset_manager(ENGINE_REPORT_DIR "/assets");
    morrow_jni_runtime_configuration();

    RealLinkResult onload = {0};
    horizon_real_link_run(&onload, 1u);
    if (!horizon_real_link_jni_onload_ready()) {
        // A reason and log path were saved before leaving the SDL preview.
        // No guest EGL entry has been attempted by this path yet.
        return failure_screen("Roblox JNI_OnLoad blocked. Read the newest link-XX.txt on SD.");
    }
    RealLinkResult bridge = {0};
    horizon_real_link_run(&bridge, 3u);
    if (!horizon_real_link_app_bridge_ready()) {
        // JNI_OnLoad can register guest callbacks and start native workers.
        // Exit the process without resurrecting the SDL/console renderer.
        return 0;
    }

    // Keep the exclusive output alive for guest-owned rendering threads.
    // A bounded periodic callback experiment runs on this same main thread,
    // not on an invented Android worker or an SDL timer. The existing bridge
    // report captures each durable pump attempt and subsequent EGL activity.
    FILE *trace = bridge.log_path[0] ? fopen(bridge.log_path, "a") : NULL;
    if (trace) {
        fprintf(trace, "exclusive_display_handoff=after_sdl_shutdown\n");
        fprintf(trace, "exclusive_guest_frames_visible=unverified\n");
        fprintf(trace, "exclusive_message_pump=bounded_120_calls_at_roughly_10hz\n");
        fflush(trace);
        fsync(fileno(trace));
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    unsigned frames = 0, sample = 0, pump_calls = 0;
    bool pump_live = trace != NULL;
    exclusive_graphics_snapshot(trace, sample++);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(16000000ULL);
        frames++;
        if (pump_live && pump_calls < 120 && frames % 6u == 0) {
            pump_calls++;
            pump_live = horizon_real_link_pump_exclusive(
                trace, bridge.log_path, pump_calls);
            if (!pump_live) {
                fprintf(trace, "exclusive_message_pump_stopped_at=%u\n", pump_calls);
                fflush(trace);
                fsync(fileno(trace));
            } else if (pump_calls == 120) {
                fprintf(trace, "exclusive_message_pump_budget_exhausted=yes\n");
                fflush(trace);
                fsync(fileno(trace));
            }
        }
        // At most ten minutes of 2-second snapshots; no unbounded SD log.
        if (frames % 120u == 0 && sample <= 300)
            exclusive_graphics_snapshot(trace, sample++);
    }
    exclusive_graphics_snapshot(trace, sample);
    if (trace) fclose(trace);
    // Engine mappings and registered callbacks are reclaimed by OS exit;
    // never destroy JNI/global refs out from under guest rendering threads.
    return 0;
}
#endif

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
        // Keep touchscreen targets aligned with the seven drawn rail rows.
        unsigned index = 0;
        if (morrow_rail_hit(x, y, UI_PAGE_COUNT, &index))
            state->page = (UiPage)index;
    } else if (state->page == UI_OVERVIEW &&
               x >= 332 && x < 555 && y >= 375 && y < 431) {
        state->page = UI_LIBRARY;
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
    SDL_Window *window = SDL_CreateWindow("Cordial Switched - Morrow UI Preview", 0, 0,
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
    // Recover a persisted entry marker from a previous terminated process.
    // This can indicate a crash, forced exit or power loss; it is not a stack
    // trace. The numbered report and the prior link log remain on SD.
    if (mkdir(ENGINE_REPORT_DIR, 0777) == 0 || errno == EEXIST)
        engine_run_journal_recover(ENGINE_REPORT_DIR, state.recovered_crash_path,
                                   sizeof(state.recovered_crash_path));

    bool quit = false;
    bool run_gles_smoke = false;
    bool run_exclusive_engine = false;
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
        if (state.page == UI_LOADER && (pressed & HidNpadButton_Minus)) {
            // Neither the synthetic triangle nor a real guest EGL launch
            // may share SDL's active renderer. Both paths begin after teardown.
            const u64 held = padGetButtons(&pad);
            run_exclusive_engine = (held & HidNpadButton_ZL) &&
                                   (held & HidNpadButton_ZR);
            run_gles_smoke = !run_exclusive_engine;
            break;
        }
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
        if (state.page == UI_LOADER && (pressed & HidNpadButton_StickR)) {
            const u64 held = padGetButtons(&pad);
            if ((held & HidNpadButton_ZL) && (held & HidNpadButton_ZR))
                horizon_real_link_run(&state.real_link, 4u);
            else
                horizon_loader_smoke_run(&state.loader_smoke);
        }
        if (state.page == UI_LOADER && (pressed & HidNpadButton_StickL)) {
#ifdef MORROW_HAS_JNIVM
            // Register a genuine Java reference to the host-owned window,
            // but do not create another EGL context while SDL is drawing.
            if (prepare_jni_runtime()) {
                morrow_jni_runtime_bind_horizon_surface(
                    nwindowGetDefault(), UI_WIDTH, UI_HEIGHT);
                morrow_jni_runtime_bind_asset_manager(
                    ENGINE_REPORT_DIR "/assets");
                morrow_jni_runtime_configuration();
            }
#endif
            const u64 held = padGetButtons(&pad);
            horizon_real_link_run(&state.real_link,
                (held & HidNpadButton_ZL) && (held & HidNpadButton_ZR) ? 3u :
                (held & HidNpadButton_ZL) ? 1u :
                (held & HidNpadButton_ZR) ? 2u : 0u);
        }

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

#ifdef MORROW_HAS_JNIVM
    if (!run_exclusive_engine) {
        // A live guest can retain these refs after JNI_OnLoad. Only the
        // normal UI/synthetic smoke path releases them proactively.
        morrow_jni_runtime_release_local_storage_manager();
        morrow_jni_runtime_unbind_asset_manager();
        morrow_jni_runtime_unbind_surface();
        morrow_jni_runtime_release_configuration();
        morrow_jni_runtime_release_game_activity();
    }
#endif
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (run_exclusive_engine) {
#ifdef MORROW_HAS_JNIVM
        return exclusive_engine_launch();
#else
        return failure_screen("Exclusive engine launch needs the JNI-enabled Morrow build.");
#endif
    }
    if (run_gles_smoke)
        morrow_gles_smoke_run();
    return 0;
}
