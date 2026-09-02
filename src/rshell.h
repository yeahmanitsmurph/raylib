/**********************************************************************************************
*
*   rshell v0.1 - Wayland layer-shell extension API for raylib (PLATFORM_WAYLAND_LAYER)
*
*   DESCRIPTION:
*       Public API to configure and manage wlr-layer-shell surfaces (bars, panels, docks,
*       OSDs, notifications) when raylib is built with PLATFORM_WAYLAND_LAYER.
*
*       A layer surface is a Wayland surface owned by the compositor shell layer stack:
*       instead of a regular window, it gets a layer (background/bottom/top/overlay),
*       anchors to output edges, margins, an exclusive zone (space reserved so other
*       windows don't overlap it) and a keyboard interactivity mode.
*
*   USAGE:
*       Call SetLayerConfig() BEFORE InitWindow(): it configures the primary surface (id 0).
*       InitWindow(width, height, title) width/height are used as size hints for axes that
*       are not stretched by anchors; the compositor configure event always wins.
*
*       Additional surfaces can be created with CreateLayerSurface() and drawn with
*       BeginSurface() (multi-surface support is declared here and implemented incrementally).
*
*   NOTES:
*       - All sizes are logical pixels; buffer size is scaled by the output (fractional) scale
*       - The header is self-contained C; LayerConfig is blittable (ints + fixed char arrays)
*         so it can be bound from other languages (C#, Zig, Odin...) without marshalling
*       - Functions are RLAPI so they are exported by name from shared builds
*
*   LICENSE: zlib/libpng
*
*   Copyright (c) 2013-2026 Ramon Santamaria (@raysan5) and contributors
*
*   This software is provided "as-is", without any express or implied warranty. In no event
*   will the authors be held liable for any damages arising from the use of this software.
*
*   Permission is granted to anyone to use this software for any purpose, including commercial
*   applications, and to alter it and redistribute it freely, subject to the following restrictions:
*
*     1. The origin of this software must not be misrepresented; you must not claim that you
*     wrote the original software. If you use this software in a product, an acknowledgment
*     in the product documentation would be appreciated but is not required.
*
*     2. Altered source versions must be plainly marked as such, and must not be misrepresented
*     as being the original software.
*
*     3. This notice may not be removed or altered from any source distribution.
*
**********************************************************************************************/

#ifndef RSHELL_H
#define RSHELL_H

#include "raylib.h"     // Required for: RLAPI, Vector2, bool

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#define RSHELL_VERSION  "0.1"

#define MAX_LAYER_SURFACES          8       // Maximum number of layer surfaces managed simultaneously
#define LAYER_OUTPUT_NAME_LENGTH    64      // Maximum length of an output (monitor) name
#define LAYER_NAMESPACE_LENGTH      64      // Maximum length of a layer surface namespace

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------

// Layer surface stacking layer (maps to zwlr_layer_shell_v1.layer)
typedef enum {
    LAYER_SHELL_BACKGROUND = 0,     // Below everything, e.g. wallpapers
    LAYER_SHELL_BOTTOM,             // Below regular windows, e.g. desktop widgets
    LAYER_SHELL_TOP,                // Above regular windows, e.g. bars and panels (default)
    LAYER_SHELL_OVERLAY             // Above everything, e.g. OSDs and lock screens
} LayerShellLayer;

// Layer surface anchor edges, bitflags (maps to zwlr_layer_surface_v1.anchor)
typedef enum {
    LAYER_SHELL_ANCHOR_NONE   = 0,  // Not anchored: floating surface, centered by the compositor
    LAYER_SHELL_ANCHOR_TOP    = 1,  // Anchor to top edge of the output
    LAYER_SHELL_ANCHOR_BOTTOM = 2,  // Anchor to bottom edge of the output
    LAYER_SHELL_ANCHOR_LEFT   = 4,  // Anchor to left edge of the output
    LAYER_SHELL_ANCHOR_RIGHT  = 8   // Anchor to right edge of the output
} LayerShellAnchor;

// Layer surface keyboard interactivity (maps to zwlr_layer_surface_v1.keyboard_interactivity)
typedef enum {
    LAYER_SHELL_KEYBOARD_NONE = 0,  // Surface never receives keyboard focus (default)
    LAYER_SHELL_KEYBOARD_EXCLUSIVE, // Surface takes exclusive keyboard focus (top/overlay layers)
    LAYER_SHELL_KEYBOARD_ON_DEMAND  // Surface receives focus when clicked, like a regular window
} LayerShellKeyboard;

// Layer surface configuration
// NOTE: Blittable struct, only ints and fixed-size char arrays (no pointers)
typedef struct LayerConfig {
    int layer;                      // Stacking layer (LayerShellLayer)
    int anchor;                     // Anchored edges (LayerShellAnchor bitflags)
    int exclusiveZone;              // Exclusive zone in logical px: >0 reserve space, 0 none, -1 ignore other zones
    int keyboard;                   // Keyboard interactivity (LayerShellKeyboard)
    int width;                      // Requested width in logical px, 0 = stretch between left/right anchors
    int height;                     // Requested height in logical px, 0 = stretch between top/bottom anchors
    int marginTop;                  // Margin from anchored top edge (logical px)
    int marginRight;                // Margin from anchored right edge (logical px)
    int marginBottom;               // Margin from anchored bottom edge (logical px)
    int marginLeft;                 // Margin from anchored left edge (logical px)
    char output[LAYER_OUTPUT_NAME_LENGTH];      // Output (monitor) name, e.g. "DP-1", empty = compositor choice
    char nameSpace[LAYER_NAMESPACE_LENGTH];     // Layer surface namespace, e.g. "bar" (used by compositor rules)
} LayerConfig;

#if defined(__cplusplus)
extern "C" {            // Prevents name mangling of functions
#endif

//------------------------------------------------------------------------------------
// Layer surface configuration (call before InitWindow() for the primary surface)
//------------------------------------------------------------------------------------
RLAPI LayerConfig GetDefaultLayerConfig(void);                          // Get default config: floating, unanchored, top layer, no exclusive zone
RLAPI void SetLayerConfig(LayerConfig config);                          // Set config for primary surface (id 0), call before InitWindow()

//------------------------------------------------------------------------------------
// Layer surface management
//------------------------------------------------------------------------------------
RLAPI int CreateLayerSurface(LayerConfig config);                       // Create an additional layer surface, returns surface id or -1 on failure
RLAPI void DestroyLayerSurface(int surface);                            // Destroy a layer surface (primary surface 0 is destroyed by CloseWindow())
RLAPI int GetLayerSurfaceCount(void);                                   // Get number of active layer surfaces
RLAPI bool IsLayerSurfaceReady(int surface);                            // Check if surface has been configured by the compositor
RLAPI bool IsLayerSurfaceClosed(int surface);                           // Check if compositor requested the surface to close
RLAPI void BeginSurface(int surface);                                   // Make surface current for drawing (use before BeginDrawing())
RLAPI int GetCurrentSurface(void);                                      // Get id of surface currently used for drawing

//------------------------------------------------------------------------------------
// Layer surface state
//------------------------------------------------------------------------------------
RLAPI void SetLayerSurfaceVisible(int surface, bool visible);           // Map/unmap surface (unmapped surfaces are not drawn and release their zone)
RLAPI bool IsLayerSurfaceVisible(int surface);                          // Check if surface is mapped
RLAPI void SetLayerSurfaceSize(int surface, int width, int height);     // Request new size (logical px), 0 = stretch along anchored axis
RLAPI void SetLayerSurfaceAnchor(int surface, int anchor);              // Set anchored edges (LayerShellAnchor bitflags)
RLAPI void SetLayerSurfaceExclusiveZone(int surface, int zone);         // Set exclusive zone (logical px)
RLAPI void SetLayerSurfaceMargins(int surface, int top, int right, int bottom, int left); // Set margins from anchored edges (logical px)
RLAPI void SetLayerSurfaceLayer(int surface, int layer);                // Set stacking layer (LayerShellLayer)
RLAPI void SetLayerSurfaceKeyboard(int surface, int keyboard);          // Set keyboard interactivity (LayerShellKeyboard)

//------------------------------------------------------------------------------------
// Layer surface geometry
//------------------------------------------------------------------------------------
RLAPI int GetLayerSurfaceWidth(int surface);                            // Get current surface width (logical px, as configured by compositor)
RLAPI int GetLayerSurfaceHeight(int surface);                           // Get current surface height (logical px, as configured by compositor)
RLAPI float GetLayerSurfaceScale(int surface);                          // Get current surface buffer scale (e.g. 1.0, 1.5, 2.0)
RLAPI int GetLayerSurfaceMonitor(int surface);                          // Get monitor index the surface is displayed on, -1 if unknown

//------------------------------------------------------------------------------------
// Input routing
//------------------------------------------------------------------------------------
RLAPI int GetPointerSurface(void);                                      // Get id of surface currently under the pointer, -1 if none
RLAPI int GetKeyboardSurface(void);                                     // Get id of surface currently holding keyboard focus, -1 if none

#if defined(__cplusplus)
}
#endif

#endif // RSHELL_H
