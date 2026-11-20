#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL_ttf.h>
#include <switch.h>

#include "engine_stage.h"
#include "ui_navigation.h"

enum { FONT_SMALL, FONT_BODY, FONT_MEDIUM, FONT_TITLE, FONT_HERO, FONT_TOTAL };
enum { MAX_LABELS = 320 };

typedef struct {
    char label[112];
    unsigned font_index;
    SDL_Texture *texture;
    int width;
    int height;
} Label;

typedef struct {
    char label[128];
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
static DynamicLabel log_labels[13];
static bool pl_started;
static bool ttf_started;

static const SDL_Color WHITE = {255, 255, 255, 255};
static const SDL_Color MUTED = {214, 213, 242, 255};
static const SDL_Color LAVENDER = {230, 188, 255, 255};
static const SDL_Color MINT = {157, 255, 199, 255};
static const SDL_Color PINK = {255, 189, 226, 255};
static const SDL_Color PANEL = {69, 62, 129, 211};
static const SDL_Color PANEL_LIGHT = {107, 91, 172, 217};

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
            const float wave = 0.5f + 0.5f * sinf(nx * 11.0f - ny * 7.0f);
            const Uint8 r = (Uint8)(71 + 47 * ny + 54 * violet + 12 * wave);
            const Uint8 g = (Uint8)(73 + 24 * ny + 31 * violet + 16 * blue);
            const Uint8 b = (Uint8)(145 + 18 * ny + 25 * violet + 33 * blue);
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
    for (unsigned i = 0; i < sizeof(log_labels) / sizeof(log_labels[0]); ++i) {
        SDL_DestroyTexture(log_labels[i].texture);
        memset(&log_labels[i], 0, sizeof(log_labels[i]));
    }
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
    rounded(renderer, 15, 16, 124, 673, 24, (SDL_Color){65, 60, 119, 199});
    rounded(renderer, 25, 35, 105, 80, 20, (SDL_Color){100, 89, 166, 166});
    rounded(renderer, 57, 49, 39, 39, 9, WHITE);
    rounded(renderer, 68, 60, 17, 17, 3, (SDL_Color){87, 78, 156, 255});
    label(renderer, 43, 91, FONT_SMALL, WHITE, "Morrow");

    const char *names[] = {"Home", "Browse", "Friends", "Avatar",
                           "Settings", "Engine", "Link", "Logs"};
    const char *glyphs[] = {"H", "?", "+", "@", "*", "E", "L", "#"};
    _Static_assert(sizeof(names) / sizeof(names[0]) == UI_PAGE_COUNT,
                   "A rail row is needed for every page");
    for (unsigned i = 0; i < UI_PAGE_COUNT; i++) {
        const int y = MORROW_RAIL_TOP + (int)i * MORROW_RAIL_STEP;
        const bool active = state->page == (UiPage)i;
        if (active) {
            rounded(renderer, 21, y, 113, MORROW_RAIL_ROW_HEIGHT, 15,
                    (SDL_Color){244, 160, 240, 109});
            rounded(renderer, 25, y + 3, 104, MORROW_RAIL_ROW_HEIGHT - 6, 13,
                    (SDL_Color){162, 102, 221, 224});
        }
        rounded(renderer, 33, y + 7, 37, 36, 11,
                active ? (SDL_Color){248, 191, 251, 255}
                       : (SDL_Color){147, 140, 199, 155});
        label(renderer, 44, y + 12, FONT_SMALL,
              active ? (SDL_Color){72, 55, 135, 255} : WHITE, glyphs[i]);
        label(renderer, 76, y + 15, FONT_SMALL, WHITE, names[i]);
    }
    label(renderer, 35, 636, FONT_SMALL, MINT, "OFFLINE");
    label(renderer, 35, 659, FONT_SMALL, MUTED, "PREVIEW");
}

static void topbar(SDL_Renderer *renderer, const char *title, const char *subtitle)
{
    label(renderer, 180, 32, FONT_TITLE, WHITE, title);
    label(renderer, 183, 79, FONT_SMALL, MUTED, subtitle);
    badge(renderer, 1062, 40, 165, "OFFLINE PREVIEW",
          (SDL_Color){112, 81, 166, 210}, WHITE);
}

static void scenic_tile(SDL_Renderer *renderer, int x, int y, int w, int h, unsigned theme)
{
    // Original vector decoration: no game thumbnails or third-party assets.
    const SDL_Color sky[] = {{123, 180, 239, 255}, {231, 151, 202, 255},
                             {139, 116, 220, 255}, {132, 207, 186, 255}};
    const SDL_Color land[] = {{106, 191, 148, 255}, {176, 115, 191, 255},
                              {121, 90, 175, 255}, {97, 160, 177, 255}};
    theme %= 4;
    rounded(renderer, x, y, w, h, 15, sky[theme]);
    rounded(renderer, x + w * 3 / 4, y + 13, 30, 30, 15,
            (SDL_Color){255, 240, 194, 255});
    rounded(renderer, x + 1, y + h * 2 / 3, w - 2, h / 3 - 1, 12, land[theme]);
    rounded(renderer, x + w / 3, y + h / 3, w / 3, h / 2, 9,
            (SDL_Color){241, 224, 235, 255});
    rounded(renderer, x + w / 3 + 14, y + h / 3 + 14, 28, 35, 5,
            (SDL_Color){146, 126, 194, 255});
    rounded(renderer, x + w / 2 + 16, y + h / 3 + 14, 27, 35, 5,
            (SDL_Color){146, 126, 194, 255});
}

static void action_card(SDL_Renderer *renderer, int x, const char *title,
                        const char *detail, unsigned colour)
{
    panel(renderer, x, 330, 252, 180, PANEL);
    scenic_tile(renderer, x + 9, 339, 234, 96, colour);
    label(renderer, x + 15, 446, FONT_BODY, WHITE, title);
    label(renderer, x + 16, 480, FONT_SMALL, MUTED, detail);
}

static void overview(SDL_Renderer *renderer, const UiState *state, double seconds)
{
    (void)seconds;
    topbar(renderer, "Morrow", "Roblox on Switch  /  Native Horizon preview");
    label(renderer, 182, 131, FONT_MEDIUM, WHITE, "Friends");
    badge(renderer, 285, 130, 160, "0 ONLINE", (SDL_Color){110, 90, 172, 215}, MINT);
    for (int i = 0; i < 8; i++) {
        const int x = 192 + i * 128;
        rounded(renderer, x, 180, 70, 70, 35, (SDL_Color){234, 178, 249, 213});
        rounded(renderer, x + 5, 185, 60, 60, 30, (SDL_Color){120, 105, 184, 255});
        rounded(renderer, x + 24, 195, 22, 22, 11, LAVENDER);
        rounded(renderer, x + 14, 221, 42, 19, 9, LAVENDER);
    }
    label(renderer, 182, 272, FONT_SMALL, MUTED,
          "Friends and live presence appear once a verified session is available.");
    label(renderer, 182, 302, FONT_MEDIUM, WHITE, "Continue building");
    action_card(renderer, 180, "Engine inspector", "Inspect libroblox.so", 0);
    action_card(renderer, 450, "Linking tests", "Real ELF diagnostics", 2);
    action_card(renderer, 720, "Settings", "Controls and graphics", 1);
    action_card(renderer, 990, "Log viewer", "Read latest SD reports", 3);

    panel(renderer, 180, 531, 1062, 100, PANEL);
    rounded(renderer, 203, 557, 15, 15, 7,
            state->engine.status == ENGINE_PROBE_INSPECTED ? MINT : PINK);
    label(renderer, 235, 544, FONT_BODY, WHITE,
          state->engine.status == ENGINE_PROBE_INSPECTED
              ? "ARM64 engine file inspected" : "Awaiting an ARM64 engine file");
    label(renderer, 235, 580, FONT_SMALL, MUTED,
          "Engine startup and Roblox gameplay are not yet verified on this Switch.");
}

static void library(SDL_Renderer *renderer, const UiState *state)
{
    topbar(renderer, "Browse", "Discover games when the Roblox runtime and account service work");
    const char *tabs[] = {"Discover", "Experiences", "People", "Groups", "Creations"};
    for (unsigned i = 0; i < 5; i++) {
        const int x = 181 + (int)i * 200;
        rounded(renderer, x, 122, 189, 45, 19,
                state->browse_tab == i ? (SDL_Color){226, 153, 244, 225} : PANEL);
        label(renderer, x + 22, 133, FONT_SMALL, WHITE, tabs[i]);
    }
    const char *categories[] = {"Trending", "Popular", "Top Rated", "New", "Up & Coming"};
    for (int i = 0; i < 5; i++) {
        const int x = 181 + i * 200;
        panel(renderer, x, 189, 187, 88, PANEL);
        label(renderer, x + 24, 218, FONT_BODY, WHITE, categories[i]);
    }
    label(renderer, 182, 299, FONT_MEDIUM, WHITE, "Preview collections");
    label(renderer, 182, 334, FONT_SMALL, MUTED,
          "Artwork below is a local UI placeholder, not a live Roblox catalogue.");
    for (int i = 0; i < 4; ++i) {
        const int x = 182 + i * 268;
        panel(renderer, x, 371, 248, 175, PANEL);
        scenic_tile(renderer, x + 9, 380, 230, 109, (unsigned)i);
        label(renderer, x + 16, 502, FONT_BODY, WHITE, "Coming soon");
    }
    panel(renderer, 181, 567, 1052, 67, PANEL);
    label(renderer, 201, 585, FONT_SMALL, PINK,
          "Browsing is offline. We will show real experiences only after engine startup and sign-in.");
}

static void accounts(SDL_Renderer *renderer)
{
    topbar(renderer, "Friends", "Roblox friends and parties will appear after authentication");
    badge(renderer, 184, 134, 122, "ALL (0)", PANEL_LIGHT, WHITE);
    badge(renderer, 318, 134, 148, "ONLINE (0)", PANEL, MUTED);
    badge(renderer, 477, 134, 158, "REQUESTS (0)", PANEL, MUTED);
    panel(renderer, 182, 192, 1055, 433, PANEL);
    for (int i = 0; i < 4; i++) {
        const int x = 389 + i * 131;
        rounded(renderer, x, 268, 82, 82, 41, (SDL_Color){217, 177, 247, 186});
        rounded(renderer, x + 25, 283, 31, 31, 16, LAVENDER);
        rounded(renderer, x + 14, 321, 54, 19, 9, LAVENDER);
    }
    label(renderer, 429, 395, FONT_TITLE, WHITE, "Your crew will be here");
    label(renderer, 329, 464, FONT_BODY, MUTED,
          "Morrow does not have a verified Roblox login or friends API yet.");
    label(renderer, 432, 503, FONT_SMALL, MINT,
          "No account credentials are collected by this preview.");
}

static void mannequin(SDL_Renderer *renderer)
{
    rounded(renderer, 1025, 252, 132, 28, 14, (SDL_Color){209, 167, 253, 132});
    rounded(renderer, 1050, 238, 82, 78, 30, (SDL_Color){209, 158, 120, 255});
    rounded(renderer, 1052, 231, 77, 26, 10, (SDL_Color){69, 45, 78, 255});
    rounded(renderer, 1044, 322, 95, 130, 18, (SDL_Color){242, 175, 221, 255});
    rounded(renderer, 1012, 327, 30, 107, 12, (SDL_Color){209, 158, 120, 255});
    rounded(renderer, 1141, 327, 30, 107, 12, (SDL_Color){209, 158, 120, 255});
    rounded(renderer, 1050, 454, 40, 104, 12, (SDL_Color){73, 75, 135, 255});
    rounded(renderer, 1095, 454, 40, 104, 12, (SDL_Color){73, 75, 135, 255});
    rounded(renderer, 1044, 547, 47, 25, 11, WHITE);
    rounded(renderer, 1094, 547, 47, 25, 11, WHITE);
    rounded(renderer, 1039, 579, 109, 13, 6, LAVENDER);
}

static void roadmap(SDL_Renderer *renderer)
{
    topbar(renderer, "Avatar", "A native wardrobe preview; real Roblox inventory is not linked");
    const char *tabs[] = {"Customize", "Clothing", "Accessories", "Animations", "Emotes"};
    for (int i = 0; i < 5; i++) {
        const int x = 181 + i * 157;
        rounded(renderer, x, 138, 148, 38, 17,
                i == 0 ? (SDL_Color){218, 151, 234, 226} : PANEL);
        label(renderer, x + 13, 145, FONT_SMALL, WHITE, tabs[i]);
    }
    panel(renderer, 181, 192, 745, 429, PANEL);
    label(renderer, 212, 210, FONT_SMALL, MUTED, "OUTFIT SLOTS  /  SAMPLE ONLY");
    const SDL_Color clothing[] = {
        {168, 153, 188, 255}, {46, 45, 65, 255}, {228, 228, 245, 255},
        {243, 165, 202, 255}, {104, 89, 136, 255}, {133, 112, 144, 255}
    };
    for (int i = 0; i < 12; ++i) {
        const int x = 205 + (i % 6) * 118;
        const int y = 274 + (i / 6) * 147;
        rounded(renderer, x, y, 103, 126, 12,
                i == 0 ? (SDL_Color){222, 163, 252, 207} : PANEL_LIGHT);
        rounded(renderer, x + 23, y + 21, 58, 83, 11, clothing[i % 6]);
        rounded(renderer, x + 13, y + 26, 21, 61, 10, clothing[i % 6]);
        rounded(renderer, x + 70, y + 26, 21, 61, 10, clothing[i % 6]);
    }
    panel(renderer, 948, 192, 290, 429, PANEL);
    mannequin(renderer);
    label(renderer, 989, 596, FONT_SMALL, MUTED, "Offline mannequin preview");
}

static void setting_line(SDL_Renderer *renderer, int y, const char *title,
                         const char *detail)
{
    rounded(renderer, 458, y, 754, 92, 16, PANEL_LIGHT);
    label(renderer, 479, y + 11, FONT_BODY, WHITE, title);
    label(renderer, 479, y + 48, FONT_SMALL, MUTED, detail);
}

static void performance(SDL_Renderer *renderer, const UiState *state)
{
    topbar(renderer, "Settings", "Personalise Morrow and troubleshoot engine linking");
    const char *categories[] = {"Account", "Controls", "Graphics", "Audio",
                                "Network", "System", "Diagnostics"};
    panel(renderer, 176, 151, 236, 470, PANEL);
    for (unsigned i = 0; i < 7; ++i) {
        const int y = 176 + (int)i * 60;
        rounded(renderer, 188, y, 212, 53, 14,
                i == state->settings_tab ? (SDL_Color){201, 137, 242, 219}
                                         : (SDL_Color){96, 82, 158, 120});
        label(renderer, 210, y + 12, FONT_BODY, WHITE, categories[i]);
    }
    panel(renderer, 432, 151, 806, 470, PANEL);
    const int tab = (int)state->settings_tab;
    label(renderer, 460, 173, FONT_MEDIUM, WHITE, categories[tab < 7 ? tab : 0]);
    if (tab == 0) {
        setting_line(renderer, 231, "Roblox account", "Not linked. Sign-in is disabled until the runtime is safe.");
        setting_line(renderer, 341, "Privacy", "No credentials, tokens or friend data stored by Morrow.");
        setting_line(renderer, 451, "Linked accounts", "No verified account integrations are available.");
    } else if (tab == 1) {
        setting_line(renderer, 231, "Nintendo Switch controls", "D-pad: navigate   |   A: select   |   B: home / back");
        setting_line(renderer, 341, "Quick navigation", "L / R: change pages   |   Touch: select a tab or card");
        setting_line(renderer, 451, "Engineering shortcuts", "See Diagnostics for isolated linker commands.");
    } else if (tab == 2) {
        char formatted[64];
        if (state->fps > 0.0) snprintf(formatted, sizeof(formatted), "%.1f FPS", state->fps);
        else snprintf(formatted, sizeof(formatted), "Sampling...");
        live_label(renderer, &fps_label, 462, 243, FONT_TITLE, MINT, formatted);
        if (state->frame_ms > 0.0)
            snprintf(formatted, sizeof(formatted), "Frame: %.2f ms", state->frame_ms);
        else snprintf(formatted, sizeof(formatted), "Frame: sampling...");
        live_label(renderer, &time_label, 462, 300, FONT_BODY, WHITE, formatted);
        label(renderer, 464, 344, FONT_SMALL, MUTED, state->renderer_name);
        label(renderer, 464, 371, FONT_SMALL, MUTED,
              state->accelerated ? "Accelerated UI renderer" : "Software UI renderer");
        label(renderer, 464, 395, FONT_SMALL, MUTED,
              state->vsync ? "VSync on" : "Manual 60 Hz frame pacing");
        rounded(renderer, 455, 475, 721, 102, 16, PANEL_LIGHT);
        label(renderer, 478, 492, FONT_BODY, WHITE, "Ambient animation");
        label(renderer, 478, 529, FONT_SMALL, MUTED, "X or touch this row to toggle");
        rounded(renderer, 1082, 506, 63, 35, 17,
                state->animations ? MINT : (SDL_Color){102, 105, 125, 255});
        rounded(renderer, state->animations ? 1109 : 1086, 510, 27, 27, 13, WHITE);
    } else if (tab == 3) {
        setting_line(renderer, 231, "Audio output", "Guest audio is not initialised. System volume applies.");
        setting_line(renderer, 341, "Sound effects", "No fake audio controls while the engine is offline.");
        setting_line(renderer, 451, "Compatibility", "Audio services are a future engine milestone.");
    } else if (tab == 4) {
        setting_line(renderer, 231, "Network", "The native UI does not connect to Roblox services.");
        setting_line(renderer, 341, "Online status", "Offline until game startup and secure authentication.");
        setting_line(renderer, 451, "Diagnostics", "Link logs are read directly from the SD card.");
    } else if (tab == 5) {
        setting_line(renderer, 231, "Display", "1280 x 720 handheld layout, capped at a 60 Hz target.");
        setting_line(renderer, 341, "Engine file", "sdmc:/switch/cordial-switched/libroblox.so");
        setting_line(renderer, 451, "Reports", "sdmc:/switch/cordial-switched/*.txt");
    } else {
        setting_line(renderer, 226, "Engine inspection", state->engine.status == ENGINE_PROBE_INSPECTED
                     ? "ARM64 ELF found. Startup is still unverified."
                     : "No valid ARM64 engine found at the expected SD path.");
        setting_line(renderer, 330, "Linker and sandbox tests",
                     "L3 linking, R3 synthetic smoke, ZL report, ZR preflight.");
        rounded(renderer, 463, 439, 347, 79, 17,
                (SDL_Color){182, 127, 226, 235});
        label(renderer, 483, 456, FONT_BODY, WHITE, "Open linker tests  >");
        rounded(renderer, 840, 439, 347, 79, 17,
                (SDL_Color){154, 129, 216, 235});
        label(renderer, 860, 456, FONT_BODY, WHITE, "Open SD log viewer  >");
        label(renderer, 466, 546, FONT_SMALL, MINT,
              "Engine and Logs also have permanent shortcuts on the left rail.");
    }
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
        else if (state->recovered_crash_path[0])
            label(renderer, 340, 611, FONT_SMALL, PINK,
                  "Prior engine attempt ended uncleanly: see crash-XX.txt on SD");
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
    } else if (state->recovered_crash_path[0]) {
        const char *base = strrchr(state->recovered_crash_path, '/');
        char message[112];
        snprintf(message, sizeof(message), "Previous native call ended uncleanly: %.55s",
                 base ? base + 1 : state->recovered_crash_path);
        label(renderer, 341, 617, FONT_SMALL, PINK, message);
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

static void log_viewer(SDL_Renderer *renderer, const UiState *state)
{
    topbar(renderer, "Engine logs", "Live SD-card diagnostic files  /  read-only viewer");
    panel(renderer, 178, 146, 302, 483, PANEL);
    label(renderer, 198, 168, FONT_BODY, WHITE, "Recent reports");
    if (!state->logs.count) {
        label(renderer, 198, 224, FONT_SMALL, PINK,
              state->logs.read_error ? "Cannot open report folder" : "No reports yet");
        label(renderer, 198, 253, FONT_SMALL, MUTED, "Run a test from Link.");
    } else {
        const unsigned first = state->logs.selected / 8 * 8;
        for (unsigned row = 0; row < 8 && first + row < state->logs.count; ++row) {
            const unsigned i = first + row;
            const int y = 213 + (int)row * 49;
            rounded(renderer, 191, y, 272, 43, 12,
                    i == state->logs.selected ? (SDL_Color){189, 133, 227, 255}
                                              : (SDL_Color){100, 84, 157, 174});
            char filename[48];
            snprintf(filename, sizeof(filename), "%.36s", state->logs.files[i].name);
            label(renderer, 201, y + 10, FONT_SMALL, WHITE, filename);
        }
    }
    panel(renderer, 494, 146, 742, 483, PANEL);
    if (state->logs.count) {
        const MorrowLogFile *selected = &state->logs.files[state->logs.selected];
        label(renderer, 518, 165, FONT_BODY, WHITE, selected->name);
        char size[70];
        snprintf(size, sizeof(size), "%lu bytes  /  last 32 KiB", selected->bytes);
        label(renderer, 519, 198, FONT_SMALL, MUTED, size);
        const unsigned visible = 13;
        const unsigned end = state->logs.line_count > state->log_scroll
            ? state->logs.line_count - state->log_scroll : 0;
        const unsigned first = end > visible ? end - visible : 0;
        for (unsigned row = 0; row < visible; ++row) {
            const unsigned line = first + row;
            const char *source = line < end ? state->logs.lines[line] : "";
            char shown[96];
            snprintf(shown, sizeof(shown), "%.88s", source);
            live_label(renderer, &log_labels[row], 518, 242 + (int)row * 27,
                       FONT_SMALL, row % 2 ? WHITE : MUTED, shown);
        }
    } else {
        label(renderer, 526, 269, FONT_MEDIUM, WHITE, "Nothing to display yet");
        label(renderer, 526, 328, FONT_BODY, MUTED,
              "Diagnostics will appear here after an on-device test.");
        label(renderer, 526, 372, FONT_SMALL, MUTED,
              "Expected examples: link-53.txt, diag-progress.txt");
    }
    label(renderer, 198, 600, FONT_SMALL, MUTED, "D-pad up/down: files");
    label(renderer, 518, 600, FONT_SMALL, MUTED,
          "D-pad left/right: scroll history   |   X / Y: refresh directory");
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
    case UI_HOME:
        overview(renderer, state, seconds);
        break;
    case UI_BROWSE:
        library(renderer, state);
        break;
    case UI_FRIENDS:
        accounts(renderer);
        break;
    case UI_AVATAR:
        roadmap(renderer);
        break;
    case UI_SETTINGS:
        performance(renderer, state);
        break;
    case UI_ENGINE:
        engine(renderer, state);
        break;
    case UI_LOADER:
        loader(renderer, state);
        break;
    case UI_LOGS:
        log_viewer(renderer, state);
        break;
    default:
        break;
    }

    rounded(renderer, 162, 648, 1080, 44, 13, (SDL_Color){79, 65, 132, 219});
    label(renderer, 178, 660, FONT_SMALL, WHITE,
          state->page == UI_LOADER
              ? "L/R pages | L3 link | ZL+ZR+Minus exclusive engine | Minus GLES | + exit"
              : "L/R pages   |   D-pad navigate   |   A select   |   B home/back   |   + exit");
    label(renderer, 1103, 660, FONT_SMALL, MINT, "Horizon");
}
