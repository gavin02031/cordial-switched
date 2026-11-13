#ifndef CORDIAL_SWITCH_UI_H
#define CORDIAL_SWITCH_UI_H

#include <stdbool.h>

#include <SDL.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720

typedef enum {
    UI_OVERVIEW = 0,
    UI_PERFORMANCE,
    UI_ROADMAP,
    UI_PAGE_COUNT
} UiPage;

typedef struct {
    UiPage page;
    bool animations;
    bool accelerated;
    bool vsync;
    double fps;
    double frame_ms;
    char renderer_name[48];
} UiState;

bool ui_init(SDL_Renderer *renderer);
void ui_draw(SDL_Renderer *renderer, const UiState *state, double seconds);
void ui_shutdown(void);

#endif
