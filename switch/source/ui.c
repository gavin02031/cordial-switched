#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL_ttf.h>
#include <switch.h>

#include "engine_stage.h"

enum { FONT_SMALL, FONT_BODY, FONT_MEDIUM, FONT_TITLE, FONT_HERO, FONT_TOTAL };
enum { MAX_LABELS = 192 };

typedef struct {
    char label[112];
    unsigned font_index;
    SDL_Texture *texture;
    int width;
    int height;
} Label;

typedef struct {
    char label[64];
    SDL_Texture *texture;
    int width;
    int height;
} DynamicLabel;

static const int font_points[FONT_TOTAL] = {16, 20, 25, 37, 54};
static TTF_Font *fonts[FONT_TOTAL];
static Label labels[MAX_LABELS];
static unsigned label_count;
static SDL_Texture *background;
static DynamicLabel fps_label;
static DynamicLabel time_label;
static bool pl_started;
static bool ttf_started;

static const SDL_Color WHITE = {243, 246, 255, 255};
static const SDL_Color MUTED = {153, 164, 190, 255};
static const SDL_Color LAVENDER = {185, 165, 255, 255};
static const SDL_Color MINT = {126, 234, 194, 255};
static const SDL_Color PINK = {255, 161, 196, 255};
static const SDL_Color PANEL = {24, 29, 51, 246};
static const SDL_Color PANEL_LIGHT = {32, 38, 65, 250};

static void fill(SDL_Renderer *renderer, int x, int y, int w, int h, SDL_Color colour)
{
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

static void rounded(SDL_Renderer *renderer, int x, int y, int w, int h,
                    int radius, SDL_Color colour)
{
    if (radius <= 0) {
        fill(renderer, x, y, w, h, colour);
        return;
    }

    // Rectangular strips avoid render targets or per-frame texture allocation.
    fill(renderer, x, y + radius, w, h - radius * 2, colour);
    fill(renderer, x + radius, y, w - radius * 2, h, colour);
    for (int row = 0; row < radius; row++) {
        const int dy = radius - row;
        const int dx = (int)sqrtf((float)(radius * radius - dy * dy));
        const int inset = radius - dx;
        fill(renderer, x + inset, y + row, w - 2 * inset, 1, colour);
        fill(renderer, x + inset, y + h - row - 1, w - 2 * inset, 1, colour);
    }
}

static void panel(SDL_Renderer *renderer, int x, int y, int w, int h, SDL_Color colour)
{
    rounded(renderer, x + 3, y + 7, w, h, 17, (SDL_Color){1, 4, 17, 82});
    rounded(renderer, x, y, w, h, 17, colour);
    fill(renderer, x + 22, y, w - 44, 1, (SDL_Color){140, 146, 200, 42});
}

static SDL_Texture *make_text(SDL_Renderer *renderer, unsigned font_index,
                              const char *string, int *width, int *height)
{
    SDL_Surface *surface = TTF_RenderUTF8_Blended(fonts[font_index], string,
                                                  (SDL_Color){255, 255, 255, 255});
    if (!surface)
        return NULL;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        *width = surface->w;
        *height = surface->h;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    SDL_FreeSurface(surface);
    return texture;
}

static void render_texture(SDL_Renderer *renderer, SDL_Texture *texture, int x, int y,
                           int w, int h, SDL_Color colour)
{
    if (!texture)
        return;
    SDL_SetTextureColorMod(texture, colour.r, colour.g, colour.b);
    SDL_SetTextureAlphaMod(texture, colour.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderCopy(renderer, texture, NULL, &rect);
}

static void label(SDL_Renderer *renderer, int x, int y, unsigned size,
                  SDL_Color colour, const char *string)
{
    Label *entry = NULL;
    for (unsigned i = 0; i < label_count; i++) {
        if (labels[i].font_index == size && strcmp(labels[i].label, string) == 0) {
            entry = &labels[i];
            break;
        }
    }

    if (!entry && label_count < MAX_LABELS) {
        entry = &labels[label_count];
        snprintf(entry->label, sizeof(entry->label), "%s", string);
        entry->font_index = size;
        entry->texture = make_text(renderer, size, string, &entry->width, &entry->height);
        if (entry->texture)
            label_count++;
    }
    if (entry)
        render_texture(renderer, entry->texture, x, y, entry->width, entry->height, colour);
}

static void live_label(SDL_Renderer *renderer, DynamicLabel *entry, int x, int y,
                       unsigned size, SDL_Color colour, const char *string)
{
    if (strcmp(entry->label, string) != 0 || !entry->texture) {
        SDL_DestroyTexture(entry->texture);
        entry->texture = make_text(renderer, size, string, &entry->width, &entry->height);
        snprintf(entry->label, sizeof(entry->label), "%s", string);
    }
    render_texture(renderer, entry->texture, x, y, entry->width, entry->height, colour);
}

static void build_background(SDL_Renderer *renderer)
{
    enum { W = 320, H = 180 };
    Uint32 *pixels = malloc(W * H * sizeof(*pixels));
    if (!pixels)
        return;

    SDL_PixelFormat *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    if (!format) {
        free(pixels);
        return;
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float nx = (float)x / W;
            const float ny = (float)y / H;
            const float violet = fmaxf(0.0f, 1.0f -
                ((nx - 0.88f) * (nx - 0.88f) * 3.1f +
                 (ny - 0.12f) * (ny - 0.12f) * 4.4f));
            const float blue = fmaxf(0.0f, 1.0f -
                ((nx - 0.46f) * (nx - 0.46f) * 4.0f +
                 (ny - 1.08f) * (ny - 1.08f) * 4.5f));
            const Uint8 r = (Uint8)(10 + 17 * ny + 27 * violet + 7 * blue);
            const Uint8 g = (Uint8)(13 + 11 * ny + 9 * violet + 18 * blue);
            const Uint8 b = (Uint8)(29 + 21 * ny + 29 * violet + 31 * blue);
            pixels[y * W + x] = SDL_MapRGBA(format, r, g, b, 255);
        }
    }
    background = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                   SDL_TEXTUREACCESS_STATIC, W, H);
    if (background) {
        SDL_UpdateTexture(background, NULL, pixels, W * (int)sizeof(*pixels));
        SDL_SetTextureBlendMode(background, SDL_BLENDMODE_NONE);
    }
    SDL_FreeFormat(format);
    free(pixels);
}

bool ui_init(SDL_Renderer *renderer)
{
    if (R_FAILED(plInitialize(PlServiceType_User)))
        return false;
    pl_started = true;

    PlFontData system_font;
    if (R_FAILED(plGetSharedFontByType(&system_font, PlSharedFontType_Standard))) {
        ui_shutdown();
        return false;
    }
    if (TTF_Init() < 0) {
        ui_shutdown();
        return false;
    }
    ttf_started = true;

    for (unsigned i = 0; i < FONT_TOTAL; i++) {
        SDL_RWops *memory = SDL_RWFromConstMem(system_font.address, (int)system_font.size);
        if (!memory) {
            ui_shutdown();
            return false;
        }
        fonts[i] = TTF_OpenFontRW(memory, 1, font_points[i]);
        if (!fonts[i]) {
            ui_shutdown();
            return false;
        }
    }
    build_background(renderer);
    return true;
}

void ui_shutdown(void)
{
    for (unsigned i = 0; i < label_count; i++)
        SDL_DestroyTexture(labels[i].texture);
    label_count = 0;
    SDL_DestroyTexture(fps_label.texture);
    SDL_DestroyTexture(time_label.texture);
    memset(&fps_label, 0, sizeof(fps_label));
    memset(&time_label, 0, sizeof(time_label));
    SDL_DestroyTexture(background);
    background = NULL;

    for (unsigned i = 0; i < FONT_TOTAL; i++) {
        if (fonts[i])
            TTF_CloseFont(fonts[i]);
        fonts[i] = NULL;
    }
    if (ttf_started) {
        TTF_Quit();
        ttf_started = false;
    }
    if (pl_started) {
        plExit();
        pl_started = false;
    }
}

static void badge(SDL_Renderer *renderer, int x, int y, int w, const char *text,
                  SDL_Color background_colour, SDL_Color text_colour)
{
    rounded(renderer, x, y, w, 34, 15, background_colour);
    label(renderer, x + 13, y + 6, FONT_SMALL, text_colour, text);
}

static void sidebar(SDL_Renderer *renderer, const UiState *state)
{
    panel(renderer, 28, 27, 246, 665, (SDL_Color){17, 23, 44, 246});
    rounded(renderer, 48, 49, 54, 54, 14, LAVENDER);
    label(renderer, 63, 59, FONT_TITLE, (SDL_Color){22, 16, 48, 255}, "C");
    label(renderer, 111, 52, FONT_MEDIUM, WHITE, "CORDIAL");
    label(renderer, 112, 81, FONT_SMALL, LAVENDER, "SWITCHED");

    label(renderer, 49, 155, FONT_SMALL, MUTED, "NAVIGATION");
    const char *items[] = {"Overview", "Performance", "Roadmap", "Engine", "Loader"};
    for (unsigned i = 0; i < UI_PAGE_COUNT; i++) {
        const int y = 229 + (int)i * 69;
        const bool active = state->page == (UiPage)i;
        if (active) {
            rounded(renderer, 44, y, 214, 58, 13, (SDL_Color){59, 49, 91, 255});
            rounded(renderer, 45, y + 13, 4, 31, 2, LAVENDER);
        }
        rounded(renderer, 63, y + 20, 15, 15, 5,
                active ? LAVENDER : (SDL_Color){83, 94, 123, 255});
        label(renderer, 91, y + 14, FONT_BODY, active ? WHITE : MUTED, items[i]);
    }

    fill(renderer, 49, 571, 200, 1, (SDL_Color){144, 150, 204, 38});
    label(renderer, 49, 593, FONT_SMALL, MUTED, "ENGINE STATUS");
    rounded(renderer, 48, 625, 180, 34, 10, (SDL_Color){66, 43, 63, 235});
    rounded(renderer, 59, 636, 9, 9, 4, PINK);
    label(renderer, 77, 631, FONT_SMALL, PINK, "NOT LOADED");
}

static void topbar(SDL_Renderer *renderer, const char *title, const char *subtitle)
{
    label(renderer, 310, 42, FONT_TITLE, WHITE, title);
    label(renderer, 312, 92, FONT_SMALL, MUTED, subtitle);
    badge(renderer, 1042, 47, 184, "NATIVE TEST  v0.35", (SDL_Color){54, 46, 83, 220},
          LAVENDER);
}

static void stat_card(SDL_Renderer *renderer, int x, const char *name,
                      const char *value, SDL_Color accent)
{
    panel(renderer, x, 475, 291, 158, PANEL);
    rounded(renderer, x + 21, 496, 31, 5, 2, accent);
    label(renderer, x + 22, 515, FONT_SMALL, MUTED, name);
    label(renderer, x + 22, 546, FONT_TITLE, WHITE, value);
    label(renderer, x + 22, 601, FONT_SMALL, accent, "Horizon homebrew");
}

static void overview(SDL_Renderer *renderer, const UiState *state, double seconds)
{
    topbar(renderer, "Welcome aboard.", "Cordial's native Switch port in development");
    panel(renderer, 306, 142, 921, 304, PANEL);

    badge(renderer, 337, 170, 169, "HORIZON OS / ARM64",
          (SDL_Color){59, 52, 100, 255}, LAVENDER);
    label(renderer, 337, 222, FONT_HERO, WHITE, "A new way to play.");
    label(renderer, 340, 299, FONT_BODY, MUTED, "The foundation for Roblox on your Switch Lite.");
    label(renderer, 340, 331, FONT_BODY, MUTED, "The game engine is not connected yet.");

    rounded(renderer, 336, 379, 179, 43, 12, (SDL_Color){111, 209, 175, 52});
    label(renderer, 352, 387, FONT_BODY, MINT, "Platform online");

    // A lightweight abstract console motif: no copyrighted Roblox imagery.
    const int motion = state->animations ? (int)(5.0 * sin(seconds * 1.25)) : 0;
    rounded(renderer, 991, 195 + motion, 155, 157, 24, (SDL_Color){59, 54, 111, 255});
    rounded(renderer, 1010, 214 + motion, 117, 119, 19, (SDL_Color){178, 162, 255, 255});
    rounded(renderer, 1031, 232 + motion, 76, 84, 11, (SDL_Color){76, 65, 134, 255});
    rounded(renderer, 1049, 252 + motion, 39, 43, 9, MINT);
    rounded(renderer, 947, 294 - motion, 54, 54, 12, (SDL_Color){244, 168, 201, 255});
    rounded(renderer, 1136, 220 - motion, 28, 28, 8, MINT);

    stat_card(renderer, 306, "OUTPUT RESOLUTION", "1280 x 720", LAVENDER);
    stat_card(renderer, 619, "UI FRAME TARGET", "60 FPS", MINT);
    stat_card(renderer, 932, "ROBLOX ENGINE", "OFFLINE", PINK);
}

static void performance(SDL_Renderer *renderer, const UiState *state)
{
    topbar(renderer, "Performance lab.", "Measured UI rendering, not Roblox game performance");
    panel(renderer, 306, 142, 558, 310, PANEL);
    label(renderer, 336, 169, FONT_SMALL, MUTED, "LIVE UI FRAME RATE");

    char formatted[64];
    if (state->fps > 0.0)
        snprintf(formatted, sizeof(formatted), "%.1f FPS", state->fps);
    else
        snprintf(formatted, sizeof(formatted), "Sampling...");
    live_label(renderer, &fps_label, 338, 223, FONT_HERO,
               state->fps >= 55.0 ? MINT : LAVENDER, formatted);

    const double proportion = fmin(1.0, fmax(0.0, state->fps / 60.0));
    rounded(renderer, 340, 316, 472, 12, 6, (SDL_Color){57, 64, 94, 255});
    if (proportion > 0.025)
        rounded(renderer, 340, 316, (int)(472 * proportion), 12, 5, MINT);
    label(renderer, 340, 351, FONT_SMALL, MUTED, "Target: 60 Hz with VSync where supported");
    label(renderer, 340, 380, FONT_SMALL, MUTED, "Measured after each rendered frame");

    panel(renderer, 882, 142, 345, 310, PANEL);
    label(renderer, 910, 168, FONT_SMALL, MUTED, "FRAME TIME (AVERAGE)");
    if (state->frame_ms > 0.0)
        snprintf(formatted, sizeof(formatted), "%.2f ms", state->frame_ms);
    else
        snprintf(formatted, sizeof(formatted), "Sampling...");
    live_label(renderer, &time_label, 909, 219, FONT_TITLE, WHITE, formatted);
    fill(renderer, 911, 282, 283, 1, (SDL_Color){160, 172, 218, 50});
    label(renderer, 911, 305, FONT_SMALL, MUTED, "RENDERER");
    label(renderer, 911, 334, FONT_BODY, state->accelerated ? MINT : PINK,
          state->accelerated ? "GPU accelerated" : "Software fallback");
    label(renderer, 911, 382, FONT_SMALL, MUTED,
          state->vsync ? "VSync active" : "Manual 60 Hz pacing");
    label(renderer, 911, 412, FONT_SMALL, MUTED, state->renderer_name);

    panel(renderer, 306, 475, 921, 159, PANEL_LIGHT);
    label(renderer, 337, 500, FONT_MEDIUM, WHITE, "Lightweight visuals");
    label(renderer, 339, 542, FONT_SMALL, MUTED, "Cache fonts and backgrounds; minimise work each frame.");
    rounded(renderer, 741, 504, 419, 77, 14,
            state->animations ? (SDL_Color){64, 63, 110, 255}
                              : (SDL_Color){53, 59, 79, 255});
    label(renderer, 759, 515, FONT_BODY, WHITE, "Ambient animation");
    label(renderer, 759, 548, FONT_SMALL, MUTED, "X or tap to toggle");
    rounded(renderer, 1084, 526, 54, 30, 15,
            state->animations ? MINT : (SDL_Color){91, 99, 116, 255});
    rounded(renderer, state->animations ? 1111 : 1087, 529, 24, 24, 12, WHITE);
}

static void roadmap_row(SDL_Renderer *renderer, int step, int y, const char *name,
                        const char *detail, bool active)
{
    rounded(renderer, 334, y, 864, 68, 13,
            active ? (SDL_Color){54, 57, 98, 255}
                   : (SDL_Color){28, 34, 59, 255});
    rounded(renderer, 352, y + 16, 36, 36, 11,
            active ? MINT : (SDL_Color){69, 75, 111, 255});
    char number[8];
    snprintf(number, sizeof(number), "%02d", step);
    label(renderer, 359, y + 21, FONT_SMALL,
          active ? (SDL_Color){18, 47, 48, 255} : WHITE, number);
    label(renderer, 410, y + 8, FONT_BODY, WHITE, name);
    label(renderer, 412, y + 39, FONT_SMALL, MUTED, detail);
    if (active)
        badge(renderer, 1019, y + 17, 149, "IN TESTING",
              (SDL_Color){41, 82, 79, 255}, MINT);
}

static void roadmap(SDL_Renderer *renderer)
{
    topbar(renderer, "Our road ahead.", "Progress towards official Android Roblox on Horizon OS");
    panel(renderer, 306, 141, 921, 498, PANEL);
    label(renderer, 338, 158, FONT_MEDIUM, WHITE, "Engineering milestones");
    roadmap_row(renderer, 1, 205, "Native Switch interface",
                "720p GPU dashboard, controls and touchscreen", true);
    roadmap_row(renderer, 2, 280, "Android ARM64 ELF loader",
                "Port relocation, mapping and symbol resolution", false);
    roadmap_row(renderer, 3, 355, "Compatibility services",
                "Bionic, JNI, audio, network and graphics", false);
    roadmap_row(renderer, 4, 430, "Roblox engine startup",
                "Load a user-provided, signed Android engine", false);
    roadmap_row(renderer, 5, 505, "Account and online games",
                "Only if Roblox accepts the unofficial runtime", false);
}

static void engine(SDL_Renderer *renderer, const UiState *state)
{
    topbar(renderer, "Engine inspection.", "Read-only metadata for a user-supplied Android ARM64 library");
    panel(renderer, 306, 142, 921, 496, PANEL);

    const bool inspected = state->engine.status == ENGINE_PROBE_INSPECTED;
    const SDL_Color accent = inspected ? MINT : PINK;
    const char *status = "NOT FOUND";
    switch (state->engine.status) {
    case ENGINE_PROBE_INSPECTED: status = "ELF STRUCTURE OK"; break;
    case ENGINE_PROBE_WRONG_ARCH: status = "WRONG CPU"; break;
    case ENGINE_PROBE_INVALID_ELF: status = "INVALID ELF"; break;
    case ENGINE_PROBE_IO_ERROR: status = "READ ERROR"; break;
    default: break;
    }

    badge(renderer, 337, 169, 191, status,
          inspected ? (SDL_Color){41, 82, 79, 255}
                    : (SDL_Color){77, 46, 68, 255}, accent);
    label(renderer, 339, 226, FONT_MEDIUM, WHITE, "libroblox.so");
    label(renderer, 340, 267, FONT_SMALL, MUTED, state->engine.detail);
    label(renderer, 339, 313, FONT_SMALL, LAVENDER,
          "sdmc:/switch/cordial-switched/libroblox.so");

    fill(renderer, 337, 356, 852, 1, (SDL_Color){144, 150, 204, 38});
    if (inspected) {
        char info[90];
        snprintf(info, sizeof(info), "File: %.1f MiB   |   Load segments: %u",
                 (double)state->engine.file_bytes / (1024.0 * 1024.0),
                 state->engine.load_segments);
        label(renderer, 339, 370, FONT_BODY, WHITE, info);
        snprintf(info, sizeof(info), "Largest segment alignment: %llu bytes",
                 (unsigned long long)state->engine.max_segment_alignment);
        label(renderer, 339, 408, FONT_SMALL, MUTED, info);
        snprintf(info, sizeof(info), "Android dependencies: %u (first %u shown)",
                 state->engine.needed_count,
                 (unsigned)ENGINE_PROBE_MAX_VISIBLE);
        label(renderer, 339, 448, FONT_SMALL, MUTED, info);

        const unsigned visible = state->engine.needed_count < ENGINE_PROBE_MAX_VISIBLE
            ? state->engine.needed_count : ENGINE_PROBE_MAX_VISIBLE;
        for (unsigned i = 0; i < visible; i++) {
            label(renderer, 340 + (int)(i % 3) * 269,
                  482 + (int)(i / 3) * 36, FONT_SMALL, LAVENDER,
                  state->engine.needed[i]);
        }
    } else {
        label(renderer, 339, 382, FONT_BODY, WHITE, "Provide the ARM64 library from your own official APK.");
        label(renderer, 340, 425, FONT_SMALL, MUTED,
              "This scanner never downloads or executes Roblox code.");
    }
    const char *jit_status = "NOT TESTED";
    switch (state->jit.status) {
    case JIT_PROBE_VERIFIED: jit_status = "VERIFIED"; break;
    case JIT_PROBE_NOT_RUN: break;
    default: jit_status = "UNAVAILABLE / FAILED"; break;
    }
    char jit_line[80];
    snprintf(jit_line, sizeof(jit_line), "JIT execution: %s", jit_status);
    label(renderer, 339, 559, FONT_SMALL,
          state->jit.status == JIT_PROBE_VERIFIED ? MINT : LAVENDER, jit_line);
    if (state->jit.status != JIT_PROBE_NOT_RUN)
        label(renderer, 339, 588, FONT_SMALL, MUTED, state->jit.detail);
    badge(renderer, 982, 568, 204, "X: TEST JIT",
          (SDL_Color){54, 46, 83, 220}, LAVENDER);
    label(renderer, 339, 613, FONT_SMALL, PINK,
          "Only our tiny ARM64 test runs. Roblox stays offline. Y: rescan.");
}

static void loader(SDL_Renderer *renderer, const UiState *state)
{
    topbar(renderer, "Loader readiness.", "ARM64 ELF inspection and isolated scratch-link test");
    panel(renderer, 306, 142, 921, 496, PANEL);
    if (state->engine.status != ENGINE_PROBE_INSPECTED) {
        badge(renderer, 337, 170, 190, "NO ENGINE PROFILE",
              (SDL_Color){77, 46, 68, 255}, PINK);
        label(renderer, 339, 247, FONT_MEDIUM, WHITE, "No valid ARM64 library inspected.");
        label(renderer, 340, 305, FONT_BODY, MUTED, "Add your own library on the SD card, then press Y.");
        label(renderer, 340, 553, FONT_SMALL, PINK, "This screen never executes external code.");
        if (state->real_link.attempted)
            label(renderer, 340, 611, FONT_SMALL, PINK, state->real_link.detail);
        else if (state->loader_smoke.status != LOADER_SMOKE_NOT_RUN)
            label(renderer, 340, 611, FONT_SMALL,
                  state->loader_smoke.status == LOADER_SMOKE_VERIFIED ? MINT : PINK,
                  state->loader_smoke.detail);
        else
            label(renderer, 340, 611, FONT_SMALL, LAVENDER,
                  "R3: synthetic ARM64 ELF mapping test (no Roblox library needed)");
        return;
    }

    badge(renderer, 337, 168, 212, "ARM64 PROFILE READY",
          (SDL_Color){41, 82, 79, 255}, MINT);
    badge(renderer, 976, 168, 212,
          state->loader_view == 0 ? "X: LIBRARIES" :
          state->loader_view == 1 ? "X: IMPORTS" : "X: PROFILE",
          (SDL_Color){54, 46, 83, 220}, LAVENDER);
    if (state->real_link.attempted) {
        const char *base = strrchr(state->real_link.log_path, '/');
        char report[112];
        snprintf(report, sizeof(report), "L3: %.58s / %.28s",
                 state->real_link.detail,
                 state->real_link.log_saved
                     ? (base ? base + 1 : state->real_link.log_path)
                     : "log not saved");
        label(renderer, 341, 617, FONT_SMALL,
              state->real_link.stage_prepared ? MINT : PINK, report);
    } else if (state->loader_smoke.status != LOADER_SMOKE_NOT_RUN) {
        const char *base = strrchr(state->loader_smoke.log_path, '/');
        char report[112];
        snprintf(report, sizeof(report), "R3: %.58s / %.28s",
                 state->loader_smoke.detail,
                 state->loader_smoke.log_saved
                     ? (base ? base + 1 : state->loader_smoke.log_path)
                     : "log not saved");
        label(renderer, 341, 617, FONT_SMALL,
              state->loader_smoke.status == LOADER_SMOKE_VERIFIED ? MINT : PINK,
              report);
    } else if (state->boot_log_attempted) {
        if (state->boot_log_saved) {
            const char *base = strrchr(state->boot_log_path, '/');
            char report[112];
            snprintf(report, sizeof(report), "ZR: saved %.32s / engine not executed",
                     base ? base + 1 : state->boot_log_path);
            label(renderer, 341, 617, FONT_SMALL, MINT, report);
        } else {
            label(renderer, 341, 617, FONT_SMALL, PINK,
                  "ZR: could not save log (check SD space and folder permissions)");
        }
    } else if (state->report_attempted) {
        if (state->report_saved) {
            const char *base = strrchr(state->report_path, '/');
            char report[112];
            snprintf(report, sizeof(report), "ZL: saved %.90s",
                     base ? base + 1 : state->report_path);
            label(renderer, 341, 617, FONT_SMALL, MINT, report);
        } else {
            label(renderer, 341, 617, FONT_SMALL, PINK,
                  "ZL: report failed (check SD free space and folder permissions)");
        }
    } else {
        label(renderer, 341, 617, FONT_SMALL, MUTED,
              "ZL: export full symbol list to sdmc:/switch/cordial-switched/");
    }

    if (state->loader_view == 1) {
        char heading[100];
        snprintf(heading, sizeof(heading), "Android dependencies: %u required",
                 state->engine.needed_count);
        label(renderer, 340, 224, FONT_MEDIUM, WHITE, heading);
        const unsigned visible = state->engine.needed_count < ENGINE_PROBE_MAX_NEEDED
            ? state->engine.needed_count : ENGINE_PROBE_MAX_NEEDED;
        for (unsigned i = 0; i < visible; i++) {
            const int col = (int)i / 6;
            const int row = (int)i % 6;
            const int x = 336 + col * 439;
            const int y = 277 + row * 48;
            rounded(renderer, x, y, 411, 41, 10, PANEL_LIGHT);
            label(renderer, x + 14, y + 7, FONT_BODY, LAVENDER,
                  state->engine.needed[i]);
        }
        label(renderer, 340, 589, FONT_SMALL, PINK,
              "These are imports, not services currently available on Horizon.");
        return;
    }

    if (state->loader_view == 2) {
        char info[112];
        snprintf(info, sizeof(info), "PLT imports: %llu from %llu relocations",
                 (unsigned long long)state->engine.plt_imports,
                 (unsigned long long)state->engine.plt_records);
        label(renderer, 340, 224, FONT_MEDIUM, WHITE, info);
        snprintf(info, sizeof(info), "Versioned: %llu   |   liblog adapters: %llu (not linked)",
                 (unsigned long long)state->engine.plt_versioned_imports,
                 (unsigned long long)state->engine.plt_log_compat_imports);
        label(renderer, 340, 259, FONT_SMALL,
              state->engine.plt_versioned_imports ? PINK : MUTED, info);
        for (unsigned i = 0; i < state->engine.captured_import_names; i++) {
            const int col = (int)i / 6;
            const int row = (int)i % 6;
            const int x = 336 + col * 439;
            const int y = 293 + row * 46;
            rounded(renderer, x, y, 411, 40, 10, PANEL_LIGHT);
            char display[44];
            snprintf(display, sizeof(display), "%.40s", state->engine.import_names[i]);
            label(renderer, x + 14, y + 9, FONT_SMALL, LAVENDER, display);
        }
        if (!state->engine.captured_import_names)
            label(renderer, 340, 363, FONT_BODY, MUTED,
                  "No enumerable PLT symbol names in this library.");
        label(renderer, 340, 589, FONT_SMALL, PINK,
              "Read-only symbol inspection. No imports linked; engine remains offline.");
        return;
    }

    rounded(renderer, 332, 218, 416, 181, 15, PANEL_LIGHT);
    label(renderer, 354, 238, FONT_SMALL, LAVENDER, "MEMORY REQUIREMENTS");
    char info[112];
    snprintf(info, sizeof(info), "Disk: %.1f MiB   |   Span: %.1f MiB",
             (double)state->engine.file_bytes / (1024.0 * 1024.0),
             (double)state->engine.virtual_image_bytes / (1024.0 * 1024.0));
    label(renderer, 354, 271, FONT_BODY, WHITE, info);
    snprintf(info, sizeof(info), "Segments: %u   |   RX: %u   |   RW: %u",
             state->engine.load_segments, state->engine.executable_segments,
             state->engine.writable_segments);
    label(renderer, 354, 314, FONT_SMALL, MUTED, info);
    snprintf(info, sizeof(info), "Largest alignment: %llu bytes",
             (unsigned long long)state->engine.max_segment_alignment);
    label(renderer, 354, 344, FONT_SMALL, MUTED, info);
    if (state->engine.virtual_image_bytes > ENGINE_STAGE_MAX_BYTES)
        label(renderer, 354, 373, FONT_SMALL, PINK, "Over current 192 MiB scratch cap");

    rounded(renderer, 762, 218, 432, 181, 15, PANEL_LIGHT);
    label(renderer, 783, 238, FONT_SMALL, LAVENDER, "DYNAMIC RELOCATIONS");
    snprintf(info, sizeof(info), "RELA: %llu   |   PLT: %llu",
             (unsigned long long)state->engine.rela_records,
             (unsigned long long)state->engine.plt_records);
    label(renderer, 783, 271, FONT_BODY, WHITE, info);
    snprintf(info, sizeof(info), "RELR words: %llu   |   Packed: %llu B",
             (unsigned long long)state->engine.relr_words,
             (unsigned long long)state->engine.android_packed_bytes);
    label(renderer, 783, 315, FONT_SMALL, MUTED, info);
    label(renderer, 783, 349, FONT_SMALL,
          state->engine.has_unsupported_plt_format ? PINK : MINT,
          state->engine.has_unsupported_plt_format
              ? "Unsupported relocation format tag present"
              : "Relocation table sizes parsed");

    fill(renderer, 335, 421, 854, 1, (SDL_Color){144, 150, 204, 38});
    label(renderer, 339, 434, FONT_MEDIUM, WHITE, "Still blocking engine startup");
    label(renderer, 341, 477, FONT_SMALL, MUTED,
          "Full bionic + JNI / GameActivity compatibility: NOT CONNECTED");
    label(renderer, 341, 508, FONT_SMALL,
          state->engine.android_packed_bytes ? PINK : MUTED,
          state->engine.android_packed_bytes
              ? "Android packed: PRESENT / scratch relocation support"
              : "Android packed relocations: not declared in this ELF");
    label(renderer, 341, 539, FONT_SMALL,
          state->engine.has_symbol_versions ? PINK : MUTED,
          state->engine.has_symbol_versions
              ? "Versioned symbols: PRESENT / GNU version reader available"
              : "Versioned symbols: not declared in this ELF");
    if (state->stage_attempted) {
        char result[140];
        snprintf(result, sizeof(result), "ZR boot preflight: %.100s", state->stage_detail);
        label(renderer, 341, 584, FONT_SMALL,
              state->stage_status == ENGINE_STAGE_PREPARED ? MINT : PINK, result);
    } else {
        label(renderer, 341, 584, FONT_SMALL, PINK,
              "ZR: boot preflight + save log (no execution; APK not verified)");
    }
}

void ui_draw(SDL_Renderer *renderer, const UiState *state, double seconds)
{
    SDL_SetRenderDrawColor(renderer, 10, 13, 30, 255);
    SDL_RenderClear(renderer);
    if (background) {
        SDL_Rect bounds = {0, 0, UI_WIDTH, UI_HEIGHT};
        SDL_RenderCopy(renderer, background, NULL, &bounds);
    }

    sidebar(renderer, state);
    switch (state->page) {
    case UI_OVERVIEW:
        overview(renderer, state, seconds);
        break;
    case UI_PERFORMANCE:
        performance(renderer, state);
        break;
    case UI_ROADMAP:
        roadmap(renderer);
        break;
    case UI_ENGINE:
        engine(renderer, state);
        break;
    case UI_LOADER:
        loader(renderer, state);
        break;
    default:
        break;
    }

    label(renderer, 312, 666, FONT_SMALL, MUTED,
          state->page == UI_LOADER
              ? "L/R: pages  |  X: views  |  ZL: list  |  ZR: audit  |  R3: ELF map  |  L3: real link  |  PLUS: exit"
              : "L/R or A/B: pages   |   X: context action   |   Y: rescan   |   PLUS: exit");
    label(renderer, 1050, 666, FONT_SMALL, LAVENDER, "60 Hz UI target");
}
