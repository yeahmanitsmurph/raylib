/*******************************************************************************************
*
*   raylib [others] example - layer-shell bar
*
*   Example complexity rating: [★☆☆☆] 1/4
*
*   Example originally created with raylib 6.0
*
*   NOTE: This example only works with raylib built for PLATFORM_WAYLAND_LAYER, it draws a
*   status bar anchored to the top edge of the output with an exclusive zone, so tiled
*   windows are placed below it (Hyprland, sway, river or any wlr-layer-shell compositor).
*
*   BUILD (from the repository root, inside a Wayland session):
*       cd src && make PLATFORM=PLATFORM_WAYLAND_LAYER && cd ..
*       gcc -std=c99 -o layer_shell_bar examples/others/layer_shell_bar.c -Isrc -Lsrc -lraylib \
*           -lwayland-client -lwayland-egl -lxkbcommon -lEGL -lGLESv2 -lpthread -lrt -lm -ldl -latomic
*       ./layer_shell_bar
*
*   NOTE: The example is standalone on purpose, it is not part of the examples Makefile lists
*   because it can not be built for other platforms
*
*   Example licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software
*
*   Copyright (c) 2026 Ramon Santamaria (@raysan5)
*
********************************************************************************************/

#include "raylib.h"
#include "rshell.h"     // Layer-shell API: LayerConfig, SetLayerConfig()

#include <time.h>       // Required for: time(), localtime(), strftime()

#define BAR_HEIGHT  28

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    // Configure the primary layer surface BEFORE InitWindow():
    // a top bar spanning the whole output width, reserving its height so windows don't overlap it
    LayerConfig config = GetDefaultLayerConfig();
    config.layer = LAYER_SHELL_TOP;
    config.anchor = LAYER_SHELL_ANCHOR_TOP | LAYER_SHELL_ANCHOR_LEFT | LAYER_SHELL_ANCHOR_RIGHT;
    config.width = 0;                   // 0 = stretch between left and right anchors
    config.height = BAR_HEIGHT;
    config.exclusiveZone = BAR_HEIGHT;  // Reserve space, tiled windows are placed below the bar
    config.keyboard = LAYER_SHELL_KEYBOARD_NONE;
    TextCopy(config.nameSpace, "raylib-bar");
    SetLayerConfig(config);

    // Width is a hint only, the compositor configure event sets the final size
    InitWindow(0, BAR_HEIGHT, "raylib [others] example - layer-shell bar");

    SetExitKey(0);      // Bars never receive keyboard focus anyway
    SetTargetFPS(30);   // Bars don't need high frame rates
    //--------------------------------------------------------------------------------------

    // Main loop
    while (!WindowShouldClose())    // Closed when the compositor closes the layer surface
    {
        // Update
        //----------------------------------------------------------------------------------
        time_t now = time(NULL);
        char clock[32] = { 0 };
        strftime(clock, sizeof(clock), "%a %d %b  %H:%M:%S", localtime(&now));

        int width = GetScreenWidth();       // Logical size assigned by the compositor
        int height = GetScreenHeight();
        Vector2 mouse = GetMousePosition(); // Surface-local logical coordinates

        bool hovered = CheckCollisionPointRec(mouse, (Rectangle){ 0, 0, (float)width, (float)height });
        //----------------------------------------------------------------------------------

        // Draw
        //----------------------------------------------------------------------------------
        BeginDrawing();

            ClearBackground(Fade(BLACK, hovered? 0.9f : 0.7f));    // Alpha works: the surface is transparent

            DrawText("raylib", 10, 4, 20, RAYWHITE);
            DrawText(TextFormat("%i x %i @ %.2fx", width, height, GetLayerSurfaceScale(0)), 100, 8, 10, LIGHTGRAY);

            int clockWidth = MeasureText(clock, 20);
            DrawText(clock, width - clockWidth - 10, 4, 20, RAYWHITE);

            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) DrawCircleV(mouse, 6, MAROON);

        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    // De-Initialization
    //--------------------------------------------------------------------------------------
    CloseWindow();      // Close layer surface and Wayland connection
    //--------------------------------------------------------------------------------------

    return 0;
}
