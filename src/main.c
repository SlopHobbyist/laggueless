#include <stdio.h>
#include "platform_win32.h"
#include "integer_scaling.h"
#include "core_loader.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Smoke test: integer-scaling math. SNES 256x224 inside 1920x1080, 4:3 target. */
    i32 rx, ry;
    me_iscale_ratios(1920, 1080, 256, 224, 4, 3, &rx, &ry);
    printf("[integer-scaling] 256x224 in 1920x1080 @ 4:3 -> rx=%d ry=%d -> %dx%d\n",
           rx, ry, 256 * rx, 224 * ry);

    HWND w = me_platform_create_window("multi-emulator (scaffold)", 1280, 720);
    if (!w) { fprintf(stderr, "window create failed\n"); return 1; }

    while (me_platform_pump()) {
        /* TODO: run one emulated frame; push audio (blocking = clock); present video. */
        Sleep(16);
    }
    return 0;
}
