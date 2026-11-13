#include <stdbool.h>
#include <stdio.h>

#include <switch.h>

// This executable exercises Horizon services before the Linux runtime is ported.
// It never loads, downloads or authenticates to the Roblox engine.
enum { PAGE_COUNT = 3 };

static void draw_screen(unsigned page, const HidTouchScreenState *touch, bool touched)
{
    consoleClear();

    printf("CORDIAL SWITCHED  |  Horizon smoke test\n");
    printf("========================================\n\n");

    switch (page) {
    case 0:
        printf("Host: Nintendo Switch / Horizon OS\n");
        printf("CPU: ARM64 (native homebrew build)\n");
        printf("Display: libnx text console\n");
        printf("Controller: handheld + standard pads\n");
        printf("Touchscreen: initialised\n\n");
        printf("Roblox engine: NOT LOADED\n");
        printf("Online login: NOT IMPLEMENTED\n\n");
        printf("This is a platform diagnostic, not a game.\n");
        break;
    case 1:
        printf("Porting milestones\n\n");
        printf("[1] NRO boot + display + input: TESTING\n");
        printf("[2] Android ARM64 ELF loader: TODO\n");
        printf("[3] Bionic / JNI / Android APIs: TODO\n");
        printf("[4] GPU + audio + networking: TODO\n");
        printf("[5] Engine init + official login: TODO\n\n");
        printf("Milestone 1 needs a real Switch test.\n");
        break;
    default:
        printf("Input diagnostics\n\n");
        printf("A / Right: next screen\n");
        printf("B / Left: previous screen\n");
        printf("Plus: exit to Homebrew Menu\n\n");
        if (touched && touch->count > 0) {
            printf("Touches: %u\n", (unsigned)touch->count);
            printf("First touch: x=%u y=%u\n",
                   (unsigned)touch->touches[0].x,
                   (unsigned)touch->touches[0].y);
        } else {
            printf("Touch the screen to check coordinates.\n");
        }
        break;
    }

    printf("\n----------------------------------------\n");
    printf("Page %u/%u    A/Right: next  B/Left: back\n",
           page + 1, (unsigned)PAGE_COUNT);
    printf("Plus: exit\n");
}

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    unsigned page = 0;
    bool redraw = true;
    bool was_touched = false;
    u32 previous_x = 0;
    u32 previous_y = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad);

        if (pressed & HidNpadButton_Plus)
            break;

        if (pressed & (HidNpadButton_A | HidNpadButton_Right)) {
            page = (page + 1) % PAGE_COUNT;
            redraw = true;
        } else if (pressed & (HidNpadButton_B | HidNpadButton_Left)) {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
            redraw = true;
        }

        HidTouchScreenState touch = {0};
        const bool touched = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;

        if (page == 2 && (touched != was_touched ||
                          (touched && (previous_x != touch.touches[0].x ||
                                       previous_y != touch.touches[0].y)))) {
            redraw = true;
        }

        if (redraw) {
            draw_screen(page, &touch, touched);
            redraw = false;
        }

        was_touched = touched;
        if (touched) {
            previous_x = touch.touches[0].x;
            previous_y = touch.touches[0].y;
        }

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
