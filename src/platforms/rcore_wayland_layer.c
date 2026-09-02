/**********************************************************************************************
*
*   rcore_wayland_layer - Functions to manage window, graphics device and inputs
*
*   PLATFORM: WAYLAND_LAYER
*       - Native Wayland client using the wlr-layer-shell protocol (zwlr_layer_surface_v1)
*       - Targets wlroots-based compositors (Hyprland, sway, river, ...) and any other
*         compositor exposing zwlr_layer_shell_v1
*       - Intended for desktop-shell components: bars, panels, docks, OSDs, notifications
*         (surfaces with a layer, anchors, margins, an exclusive zone and a keyboard mode
*         instead of a regular tiled/floating toplevel window)
*
*   LIMITATIONS:
*       - Single layer surface only for now (multi-surface API is declared in rshell.h)
*       - No fullscreen/maximize/minimize/window position (concepts don't apply to layer surfaces)
*       - No window icons, no gamepad, no touch, no clipboard yet
*
*   POSSIBLE IMPROVEMENTS:
*       - Multiple layer surfaces (one per output) with BeginSurface() switching
*       - Keyboard input through xkbcommon
*       - Cursor shape support (wp_cursor_shape_v1)
*
*   ADDITIONAL NOTES:
*       - TRACELOG() function is located in raylib [utils] module
*       - Sizes are expressed in logical pixels, the EGL buffer is scaled by the
*         compositor preferred (fractional) scale, following FLAG_WINDOW_HIGHDPI conventions
*
*   CONFIGURATION:
*       #define RCORE_PLATFORM_CUSTOM_FLAG
*           Custom flag for rcore on target platform -not used-
*
*   DEPENDENCIES:
*       - wayland-client: Wayland display connection, registry and core protocol objects
*       - wayland-egl: wl_egl_window bridge between wl_surface and EGL
*       - EGL: OpenGL ES context creation on the Wayland surface
*       - wlr-layer-shell-unstable-v1, xdg-shell, viewporter, fractional-scale-v1 protocols
*         (XML files vendored in src/external/wayland-layer/protocols, headers generated with wayland-scanner)
*       - gestures: Gestures system for touch-ready devices (or simulated from mouse inputs)
*
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

#include "rshell.h"         // Layer-shell public API: LayerConfig and surface functions

#include <string.h>         // Required for: strncpy(), strcmp(), memset()
#include <unistd.h>         // Required for: close()
#include <poll.h>           // Required for: poll() on the Wayland display fd

#include <wayland-client.h> // Wayland client library: display, registry, core protocol objects
#include <wayland-egl.h>    // Wayland EGL bridge: wl_egl_window

#include <EGL/egl.h>        // Native platform windowing system interface
#include <EGL/eglext.h>     // EGL extensions

// Generated protocol headers and marshalling code (see src/Makefile wayland-scanner rules)
// NOTE: The *-code.h files are included directly so no extra object files are required
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-client-protocol-code.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol-code.h"
#include "viewporter-client-protocol.h"
#include "viewporter-client-protocol-code.h"
#include "fractional-scale-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol-code.h"

#ifndef EGL_PLATFORM_WAYLAND_KHR
    #define EGL_PLATFORM_WAYLAND_KHR  0x31D8
#endif

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#define MAX_WAYLAND_OUTPUTS         8       // Maximum number of tracked outputs (monitors)

#define WL_COMPOSITOR_BIND_VERSION  4       // wl_compositor v4: wl_surface.set_buffer_scale/damage_buffer (v6 used when available)
#define WL_SEAT_BIND_VERSION        5       // wl_seat v5: wl_pointer.frame and axis_source events
#define WL_OUTPUT_BIND_VERSION      4       // wl_output v4: name/description events (v2 minimum for scale/done)
#define WL_LAYER_SHELL_BIND_VERSION 4       // zwlr_layer_shell_v1 v4: keyboard_interactivity on_demand, set_layer

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------

// Wayland output (monitor) data, filled by wl_output events
typedef struct {
    struct wl_output *output;       // Output protocol object
    uint32_t globalName;            // Registry global name (used on global_remove)
    char name[LAYER_OUTPUT_NAME_LENGTH]; // Connector name (e.g. "DP-1"), requires wl_output v4
    char description[128];          // Human readable description (make/model), requires wl_output v4
    int x;                          // Position in the global compositor space (logical px)
    int y;
    int width;                      // Current mode size (physical px)
    int height;
    int physicalWidth;              // Physical size in millimetres
    int physicalHeight;
    int refreshRate;                // Current mode refresh rate (mHz)
    int scale;                      // Integer scale factor (fallback when fractional scale is unavailable)
    bool done;                      // All properties received at least once (wl_output.done)
} WaylandOutput;

typedef struct {
    // Wayland connection and globals
    struct wl_display *display;             // Display connection
    struct wl_registry *registry;           // Global objects registry
    struct wl_compositor *compositor;       // Surface factory
    struct zwlr_layer_shell_v1 *layerShell; // Layer-shell global (required)
    struct wl_seat *seat;                   // Input seat
    struct xdg_wm_base *wmBase;             // xdg-shell base (required by layer-shell popups, optional here)
    struct wp_fractional_scale_manager_v1 *fractionalScaleManager; // Fractional scale (optional)
    struct wp_viewporter *viewporter;       // Viewporter (optional, required for fractional scale)

    WaylandOutput outputs[MAX_WAYLAND_OUTPUTS]; // Tracked outputs (monitors)
    int outputCount;                        // Number of tracked outputs
    int currentOutput;                      // Output index the primary surface is displayed on (-1 = unknown)

    // Primary layer surface (id 0)
    struct wl_surface *wlSurface;           // Wayland surface
    struct zwlr_layer_surface_v1 *layerSurface; // Layer-shell role object
    struct wp_fractional_scale_v1 *fractionalScale; // Fractional scale object (optional)
    struct wp_viewport *viewport;           // Viewport, maps the scaled buffer to the logical size (optional)
    struct wl_egl_window *eglWindow;        // EGL native window bound to wlSurface (created with the graphics device)
    bool configured;                        // First configure event received and acknowledged
    int logicalWidth;                       // Surface size assigned by the compositor (logical px)
    int logicalHeight;
    double scale;                           // Current buffer scale (fractional when available)
    int preferredBufferScale;               // Integer scale suggested by wl_surface.preferred_buffer_scale (0 = none)

    // EGL graphics device
    EGLDisplay device;      // Native display device (physical screen connection)
    EGLSurface surface;     // Surface to draw on, framebuffers (connected to context)
    EGLContext context;     // Graphic context, mode in which drawing can be done
    EGLConfig config;       // Graphic config
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context

static PlatformData platform = { 0 };   // Platform specific data

// Layer surface configuration for the primary surface (id 0), set before InitWindow()
static LayerConfig layerConfig = {
    .layer = LAYER_SHELL_TOP,
    .anchor = LAYER_SHELL_ANCHOR_NONE,
    .exclusiveZone = 0,
    .keyboard = LAYER_SHELL_KEYBOARD_NONE,
    .width = 0,
    .height = 0,
    .marginTop = 0, .marginRight = 0, .marginBottom = 0, .marginLeft = 0,
    .output = "",
    .nameSpace = "raylib"
};

//----------------------------------------------------------------------------------
// Module Internal Functions Declaration
//----------------------------------------------------------------------------------
int InitPlatform(void);          // Initialize platform (graphics, inputs and more)
void ClosePlatform(void);        // Close platform

static int InitWaylandConnection(void);     // Connect to the Wayland display and bind required globals
static void CloseWaylandConnection(void);   // Release globals and disconnect from the Wayland display
static int InitLayerSurface(void);          // Create the primary layer surface from layerConfig and wait for configure
static void CloseLayerSurface(void);        // Destroy the primary layer surface objects
static void UpdateSurfaceGeometry(void);    // Recompute buffer size from logical size and scale, update CORE.Window
static int GetOutputIndex(struct wl_output *output);   // Get tracked output index from protocol object (-1 if unknown)
static struct wl_output *FindOutputByName(const char *name); // Get output protocol object from connector name (NULL if not found)

// Layer surface listener callbacks
static void LayerSurfaceConfigureCallback(void *data, struct zwlr_layer_surface_v1 *layerSurface, uint32_t serial, uint32_t width, uint32_t height);
static void LayerSurfaceClosedCallback(void *data, struct zwlr_layer_surface_v1 *layerSurface);

// Wayland surface listener callbacks
static void SurfaceEnterCallback(void *data, struct wl_surface *surface, struct wl_output *output);
static void SurfaceLeaveCallback(void *data, struct wl_surface *surface, struct wl_output *output);
static void SurfacePreferredBufferScaleCallback(void *data, struct wl_surface *surface, int32_t factor);
static void SurfacePreferredBufferTransformCallback(void *data, struct wl_surface *surface, uint32_t transform);

// Fractional scale listener callbacks
static void FractionalScalePreferredScaleCallback(void *data, struct wp_fractional_scale_v1 *fractionalScale, uint32_t scale);

// Wayland registry listener callbacks
static void RegistryGlobalCallback(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void RegistryGlobalRemoveCallback(void *data, struct wl_registry *registry, uint32_t name);

// Wayland output listener callbacks
static void OutputGeometryCallback(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t physicalWidth, int32_t physicalHeight, int32_t subpixel, const char *make, const char *model, int32_t transform);
static void OutputModeCallback(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
static void OutputDoneCallback(void *data, struct wl_output *output);
static void OutputScaleCallback(void *data, struct wl_output *output, int32_t factor);
static void OutputNameCallback(void *data, struct wl_output *output, const char *name);
static void OutputDescriptionCallback(void *data, struct wl_output *output, const char *description);

// xdg_wm_base listener callbacks
static void WmBasePingCallback(void *data, struct xdg_wm_base *wmBase, uint32_t serial);

//----------------------------------------------------------------------------------
// Wayland Listeners
//----------------------------------------------------------------------------------
static const struct wl_registry_listener registryListener = {
    .global = RegistryGlobalCallback,
    .global_remove = RegistryGlobalRemoveCallback
};

static const struct wl_output_listener outputListener = {
    .geometry = OutputGeometryCallback,
    .mode = OutputModeCallback,
    .done = OutputDoneCallback,
    .scale = OutputScaleCallback,
    .name = OutputNameCallback,
    .description = OutputDescriptionCallback
};

static const struct xdg_wm_base_listener wmBaseListener = {
    .ping = WmBasePingCallback
};

static const struct zwlr_layer_surface_v1_listener layerSurfaceListener = {
    .configure = LayerSurfaceConfigureCallback,
    .closed = LayerSurfaceClosedCallback
};

static const struct wl_surface_listener surfaceListener = {
    .enter = SurfaceEnterCallback,
    .leave = SurfaceLeaveCallback,
    .preferred_buffer_scale = SurfacePreferredBufferScaleCallback,
    .preferred_buffer_transform = SurfacePreferredBufferTransformCallback
};

static const struct wp_fractional_scale_v1_listener fractionalScaleListener = {
    .preferred_scale = FractionalScalePreferredScaleCallback
};

//----------------------------------------------------------------------------------
// Module Functions Declaration
//----------------------------------------------------------------------------------
// NOTE: Functions declaration is provided by raylib.h

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------

// Check if application should close
bool WindowShouldClose(void)
{
    if (CORE.Window.ready) return CORE.Window.shouldClose;
    else return true;
}

// Toggle fullscreen mode
void ToggleFullscreen(void)
{
    TRACELOG(LOG_WARNING, "ToggleFullscreen() not available on target platform");
}

// Toggle borderless windowed mode
void ToggleBorderlessWindowed(void)
{
    TRACELOG(LOG_WARNING, "ToggleBorderlessWindowed() not available on target platform");
}

// Set window state: maximized, if resizable
void MaximizeWindow(void)
{
    TRACELOG(LOG_WARNING, "MaximizeWindow() not available on target platform");
}

// Set window state: minimized
void MinimizeWindow(void)
{
    TRACELOG(LOG_WARNING, "MinimizeWindow() not available on target platform");
}

// Restore window from being minimized/maximized
void RestoreWindow(void)
{
    TRACELOG(LOG_WARNING, "RestoreWindow() not available on target platform");
}

// Set window configuration state using flags
void SetWindowState(unsigned int flags)
{
    TRACELOG(LOG_WARNING, "SetWindowState() not available on target platform");
}

// Clear window configuration state flags
void ClearWindowState(unsigned int flags)
{
    TRACELOG(LOG_WARNING, "ClearWindowState() not available on target platform");
}

// Set icon for window
void SetWindowIcon(Image image)
{
    TRACELOG(LOG_WARNING, "SetWindowIcon() not available on target platform");
}

// Set icon for window
void SetWindowIcons(Image *images, int count)
{
    TRACELOG(LOG_WARNING, "SetWindowIcons() not available on target platform");
}

// Set title for window
void SetWindowTitle(const char *title)
{
    CORE.Window.title = title;
}

// Set window position on screen (windowed mode)
void SetWindowPosition(int x, int y)
{
    TRACELOG(LOG_WARNING, "SetWindowPosition() not available on target platform");
}

// Set monitor for the current window
void SetWindowMonitor(int monitor)
{
    TRACELOG(LOG_WARNING, "SetWindowMonitor() not available on target platform");
}

// Set window minimum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMinSize(int width, int height)
{
    CORE.Window.screenMin.width = width;
    CORE.Window.screenMin.height = height;
}

// Set window maximum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMaxSize(int width, int height)
{
    CORE.Window.screenMax.width = width;
    CORE.Window.screenMax.height = height;
}

// Set window dimensions
void SetWindowSize(int width, int height)
{
    TRACELOG(LOG_WARNING, "SetWindowSize() not available on target platform");
}

// Set window opacity, value opacity is between 0.0 and 1.0
void SetWindowOpacity(float opacity)
{
    TRACELOG(LOG_WARNING, "SetWindowOpacity() not available on target platform");
}

// Set window focused
void SetWindowFocused(void)
{
    TRACELOG(LOG_WARNING, "SetWindowFocused() not available on target platform");
}

// Get native window handle
void *GetWindowHandle(void)
{
    TRACELOG(LOG_WARNING, "GetWindowHandle() not implemented on target platform");
    return NULL;
}

// Get number of monitors
int GetMonitorCount(void)
{
    return platform.outputCount;
}

// Get current monitor where window is placed
// NOTE: Tracked from wl_surface.enter, falls back to the first output
int GetCurrentMonitor(void)
{
    if ((platform.currentOutput >= 0) && (platform.currentOutput < platform.outputCount)) return platform.currentOutput;
    return 0;
}

// Get selected monitor position
// NOTE: Position in the global compositor space (logical px)
Vector2 GetMonitorPosition(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return (Vector2){ 0, 0 };
    }

    return (Vector2){ (float)platform.outputs[monitor].x, (float)platform.outputs[monitor].y };
}

// Get selected monitor width (currently used by monitor)
// NOTE: Current mode width (physical px)
int GetMonitorWidth(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return 0;
    }

    return platform.outputs[monitor].width;
}

// Get selected monitor height (currently used by monitor)
// NOTE: Current mode height (physical px)
int GetMonitorHeight(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return 0;
    }

    return platform.outputs[monitor].height;
}

// Get selected monitor physical width in millimetres
int GetMonitorPhysicalWidth(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return 0;
    }

    return platform.outputs[monitor].physicalWidth;
}

// Get selected monitor physical height in millimetres
int GetMonitorPhysicalHeight(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return 0;
    }

    return platform.outputs[monitor].physicalHeight;
}

// Get selected monitor refresh rate
// NOTE: wl_output reports mHz, rounded to Hz
int GetMonitorRefreshRate(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return 0;
    }

    return (platform.outputs[monitor].refreshRate + 500)/1000;
}

// Get the human-readable, UTF-8 encoded name of the selected monitor
// NOTE: Returns the connector name (e.g. "DP-1") when available (wl_output v4),
// otherwise the make/model description
const char *GetMonitorName(int monitor)
{
    if ((monitor < 0) || (monitor >= platform.outputCount))
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to find selected monitor");
        return "";
    }

    if (platform.outputs[monitor].name[0] != '\0') return platform.outputs[monitor].name;
    return platform.outputs[monitor].description;
}

// Get window position XY on monitor
Vector2 GetWindowPosition(void)
{
    TRACELOG(LOG_WARNING, "GetWindowPosition() not implemented on target platform");
    return (Vector2){ 0, 0 };
}

// Get window scale DPI factor for current monitor
// NOTE: Buffer scale of the primary surface (fractional when the compositor supports it)
Vector2 GetWindowScaleDPI(void)
{
    float scale = (platform.scale > 0.0)? (float)platform.scale : 1.0f;
    return (Vector2){ scale, scale };
}

// Set clipboard text content
void SetClipboardText(const char *text)
{
    TRACELOG(LOG_WARNING, "SetClipboardText() not implemented on target platform");
}

// Get clipboard text content
// NOTE: returned string is allocated and freed by GLFW
const char *GetClipboardText(void)
{
    TRACELOG(LOG_WARNING, "GetClipboardText() not implemented on target platform");
    return NULL;
}

// Get clipboard image
Image GetClipboardImage(void)
{
    Image image = { 0 };

    TRACELOG(LOG_WARNING, "GetClipboardImage() not implemented on target platform");

    return image;
}

// Show mouse cursor
void ShowCursor(void)
{
    CORE.Input.Mouse.cursorHidden = false;
}

// Hide mouse cursor
void HideCursor(void)
{
    CORE.Input.Mouse.cursorHidden = true;
}

// Enable cursor (unlock cursor)
void EnableCursor(void)
{
    // Set cursor position in the middle
    SetMousePosition(CORE.Window.screen.width/2, CORE.Window.screen.height/2);

    CORE.Input.Mouse.cursorHidden = false;
}

// Disable cursor (lock cursor)
void DisableCursor(void)
{
    // Set cursor position in the middle
    SetMousePosition(CORE.Window.screen.width/2, CORE.Window.screen.height/2);

    CORE.Input.Mouse.cursorHidden = true;
}

// Swap back buffer with front buffer (screen drawing)
void SwapScreenBuffer(void)
{
    eglSwapBuffers(platform.device, platform.surface);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------

// Get elapsed time measure in seconds since InitTimer()
double GetTime(void)
{
    double time = 0.0;

    struct timespec ts = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long nanoSeconds = (unsigned long long)ts.tv_sec*1000000000LLU + (unsigned long long)ts.tv_nsec;

    time = (double)(nanoSeconds - CORE.Time.base)*1e-9; // Elapsed time since InitTimer()

    return time;
}

// Open URL with default system browser (if available)
// WARNING: This function is only safe to use if you control the URL given,
// a user could craft a malicious string to perform and undesired action
// NOTE: Some safety checks have been added to mitigate security issues
void OpenURL(const char *url)
{
    // Security check to (partially) avoid malicious code
    if ((strchr(url, '\'') != NULL) || (strchr(url, '\"') != NULL))
    {
        // Filter characters: ' and "
        TRACELOG(LOG_WARNING, "SYSTEM: Provided URL could be potentially malicious, avoid [\'\"] characters");
    }
    else if ((strncmp(url, "http://", 7) != 0) && (strncmp(url, "https://", 8) != 0))
    {
        // Only allow URL starting with "http://" or "https://" protocols
        TRACELOG(LOG_WARNING, "SYSTEM: Provided URL must start with 'http://' or 'https://' protocols");
    }
    else
    {
        // TODO: Load url using default browser
    }
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Inputs
//----------------------------------------------------------------------------------

// Set internal gamepad mappings
int SetGamepadMappings(const char *mappings)
{
    TRACELOG(LOG_WARNING, "SetGamepadMappings() not implemented on target platform");
    return 0;
}

// Set gamepad vibration
void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    TRACELOG(LOG_WARNING, "SetGamepadVibration() not implemented on target platform");
}

// Set mouse position XY
void SetMousePosition(int x, int y)
{
    CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
}

// Set mouse cursor
void SetMouseCursor(int cursor)
{
    TRACELOG(LOG_WARNING, "SetMouseCursor() not implemented on target platform");
}

// Get physical key name.
const char *GetKeyName(int key)
{
    TRACELOG(LOG_WARNING, "GetKeyName() not implemented on target platform");
    return "";
}

// Register all input events
void PollInputEvents(void)
{
#if SUPPORT_GESTURES_SYSTEM
    // NOTE: Gestures update must be called every frame to reset gestures correctly
    // because ProcessGestureEvent() is called on an event, not every frame
    UpdateGestures();
#endif

    // Reset keys/chars pressed registered
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;

    // Reset key repeats
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;

    // Reset last gamepad button/axis registered state
    CORE.Input.Gamepad.lastButtonPressed = 0; // GAMEPAD_BUTTON_UNKNOWN
    //CORE.Input.Gamepad.axisCount = 0;

    // Register previous touch states
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.Input.Touch.previousTouchState[i] = CORE.Input.Touch.currentTouchState[i];

    // Reset touch positions
    // TODO: It resets on target platform the mouse position and not filled again until a move-event,
    // so, if mouse is not moved it returns a (0, 0) position... this behaviour should be reviewed!
    //for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.Input.Touch.position[i] = (Vector2){ 0, 0 };

    // Register previous keys states
    // NOTE: Android supports up to 260 keys
    for (int i = 0; i < 260; i++)
    {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }

    // TODO: Poll input events for current platform
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Layer-shell API (rshell.h)
//----------------------------------------------------------------------------------
// NOTE: Only the primary surface (id 0) is managed for now, multi-surface support
// is declared in rshell.h and reported as not implemented yet

// Get default layer config: floating, unanchored, top layer, no exclusive zone
LayerConfig GetDefaultLayerConfig(void)
{
    LayerConfig config = { 0 };

    config.layer = LAYER_SHELL_TOP;
    config.anchor = LAYER_SHELL_ANCHOR_NONE;
    config.exclusiveZone = 0;
    config.keyboard = LAYER_SHELL_KEYBOARD_NONE;
    config.width = 0;
    config.height = 0;
    config.marginTop = 0;
    config.marginRight = 0;
    config.marginBottom = 0;
    config.marginLeft = 0;
    config.output[0] = '\0';
    strncpy(config.nameSpace, "raylib", LAYER_NAMESPACE_LENGTH - 1);

    return config;
}

// Set config for primary surface (id 0), must be called before InitWindow()
void SetLayerConfig(LayerConfig config)
{
    if (CORE.Window.ready)
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerConfig() must be called before InitWindow(), config ignored");
        return;
    }

    layerConfig = config;

    // Make sure strings are always terminated, config may come from foreign bindings
    layerConfig.output[LAYER_OUTPUT_NAME_LENGTH - 1] = '\0';
    layerConfig.nameSpace[LAYER_NAMESPACE_LENGTH - 1] = '\0';
    if (layerConfig.nameSpace[0] == '\0') strncpy(layerConfig.nameSpace, "raylib", LAYER_NAMESPACE_LENGTH - 1);
}

// Create an additional layer surface
int CreateLayerSurface(LayerConfig config)
{
    TRACELOG(LOG_WARNING, "LAYER: CreateLayerSurface() not implemented yet, only primary surface (0) available");
    return -1;
}

// Destroy a layer surface
void DestroyLayerSurface(int surface)
{
    TRACELOG(LOG_WARNING, "LAYER: DestroyLayerSurface() not implemented yet, primary surface is destroyed by CloseWindow()");
}

// Get number of active layer surfaces
int GetLayerSurfaceCount(void)
{
    return CORE.Window.ready? 1 : 0;
}

// Check if surface has been configured by the compositor
bool IsLayerSurfaceReady(int surface)
{
    if (surface != 0) return false;
    return platform.configured;
}

// Check if compositor requested the surface to close
bool IsLayerSurfaceClosed(int surface)
{
    if (surface != 0) return true;
    return CORE.Window.shouldClose;
}

// Make surface current for drawing
void BeginSurface(int surface)
{
    if (surface != 0) TRACELOG(LOG_WARNING, "LAYER: BeginSurface() not implemented yet for surface %i, drawing to primary surface", surface);
}

// Get id of surface currently used for drawing
int GetCurrentSurface(void)
{
    return 0;
}

// Map/unmap surface
void SetLayerSurfaceVisible(int surface, bool visible)
{
    TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceVisible() not implemented yet");
}

// Check if surface is mapped
bool IsLayerSurfaceVisible(int surface)
{
    if (surface != 0) return false;
    return CORE.Window.ready;
}

// Request new size (logical px)
// NOTE: Requests are double-buffered by the compositor, they take effect on the next
// wl_surface commit (next frame) and the compositor answers with a configure event
void SetLayerSurfaceSize(int surface, int width, int height)
{
    if ((surface != 0) || (platform.layerSurface == NULL))
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceSize() only available for primary surface (0)");
        return;
    }

    layerConfig.width = width;
    layerConfig.height = height;
    zwlr_layer_surface_v1_set_size(platform.layerSurface, (uint32_t)((width > 0)? width : 0), (uint32_t)((height > 0)? height : 0));
    wl_surface_commit(platform.wlSurface);
}

// Set anchored edges
void SetLayerSurfaceAnchor(int surface, int anchor)
{
    if ((surface != 0) || (platform.layerSurface == NULL))
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceAnchor() only available for primary surface (0)");
        return;
    }

    layerConfig.anchor = anchor;
    zwlr_layer_surface_v1_set_anchor(platform.layerSurface, (uint32_t)anchor);
    wl_surface_commit(platform.wlSurface);
}

// Set exclusive zone (logical px)
void SetLayerSurfaceExclusiveZone(int surface, int zone)
{
    if ((surface != 0) || (platform.layerSurface == NULL))
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceExclusiveZone() only available for primary surface (0)");
        return;
    }

    layerConfig.exclusiveZone = zone;
    zwlr_layer_surface_v1_set_exclusive_zone(platform.layerSurface, zone);
    wl_surface_commit(platform.wlSurface);
}

// Set margins from anchored edges (logical px)
void SetLayerSurfaceMargins(int surface, int top, int right, int bottom, int left)
{
    if ((surface != 0) || (platform.layerSurface == NULL))
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceMargins() only available for primary surface (0)");
        return;
    }

    layerConfig.marginTop = top;
    layerConfig.marginRight = right;
    layerConfig.marginBottom = bottom;
    layerConfig.marginLeft = left;
    zwlr_layer_surface_v1_set_margin(platform.layerSurface, top, right, bottom, left);
    wl_surface_commit(platform.wlSurface);
}

// Set stacking layer
// NOTE: Requires zwlr_layer_shell_v1 v2
void SetLayerSurfaceLayer(int surface, int layer)
{
    if ((surface != 0) || (platform.layerSurface == NULL))
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceLayer() only available for primary surface (0)");
        return;
    }

    if (zwlr_layer_surface_v1_get_version(platform.layerSurface) < ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION)
    {
        TRACELOG(LOG_WARNING, "LAYER: Compositor layer-shell version does not support changing the layer after creation");
        return;
    }

    layerConfig.layer = layer;
    zwlr_layer_surface_v1_set_layer(platform.layerSurface, (uint32_t)layer);
    wl_surface_commit(platform.wlSurface);
}

// Set keyboard interactivity
// NOTE: LAYER_SHELL_KEYBOARD_ON_DEMAND requires zwlr_layer_shell_v1 v4, downgraded to EXCLUSIVE otherwise
void SetLayerSurfaceKeyboard(int surface, int keyboard)
{
    if ((surface != 0) || (platform.layerSurface == NULL))
    {
        TRACELOG(LOG_WARNING, "LAYER: SetLayerSurfaceKeyboard() only available for primary surface (0)");
        return;
    }

    if ((keyboard == LAYER_SHELL_KEYBOARD_ON_DEMAND) && (zwlr_layer_surface_v1_get_version(platform.layerSurface) < 4))
    {
        TRACELOG(LOG_WARNING, "LAYER: Compositor layer-shell version does not support on-demand keyboard, using exclusive");
        keyboard = LAYER_SHELL_KEYBOARD_EXCLUSIVE;
    }

    layerConfig.keyboard = keyboard;
    zwlr_layer_surface_v1_set_keyboard_interactivity(platform.layerSurface, (uint32_t)keyboard);
    wl_surface_commit(platform.wlSurface);
}

// Get current surface width (logical px)
int GetLayerSurfaceWidth(int surface)
{
    if (surface != 0) return 0;
    return CORE.Window.screen.width;
}

// Get current surface height (logical px)
int GetLayerSurfaceHeight(int surface)
{
    if (surface != 0) return 0;
    return CORE.Window.screen.height;
}

// Get current surface buffer scale
float GetLayerSurfaceScale(int surface)
{
    if (surface != 0) return 1.0f;
    return (float)platform.scale;
}

// Get monitor index the surface is displayed on
int GetLayerSurfaceMonitor(int surface)
{
    if (surface != 0) return -1;
    return GetCurrentMonitor();
}

// Get id of surface currently under the pointer
int GetPointerSurface(void)
{
    return CORE.Input.Mouse.cursorOnScreen? 0 : -1;
}

// Get id of surface currently holding keyboard focus
int GetKeyboardSurface(void)
{
    TRACELOG(LOG_WARNING, "LAYER: GetKeyboardSurface() not implemented yet");
    return -1;
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition
//----------------------------------------------------------------------------------

// Initialize platform: graphics, inputs and more
int InitPlatform(void)
{
    platform.currentOutput = -1;
    platform.scale = 1.0;
    platform.device = EGL_NO_DISPLAY;
    platform.surface = EGL_NO_SURFACE;
    platform.context = EGL_NO_CONTEXT;

    // Layer surfaces always work in logical pixels with a scaled buffer,
    // following raylib high-DPI conventions (screen = logical, render = physical)
    FLAG_SET(CORE.Window.flags, FLAG_WINDOW_HIGHDPI);

    // Connect to the Wayland display and bind required globals
    //----------------------------------------------------------------------------
    if (InitWaylandConnection() != 0)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: Failed to connect to Wayland display");
        CloseWaylandConnection();
        return -1;
    }
    //----------------------------------------------------------------------------

    // Create the layer surface from LayerConfig and wait for the compositor configure
    //----------------------------------------------------------------------------
    if (InitLayerSurface() != 0)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: Failed to create layer surface");
        CloseLayerSurface();
        CloseWaylandConnection();
        return -1;
    }
    //----------------------------------------------------------------------------

    // TODO: Initialize graphics device (EGL context on the layer surface)
    //----------------------------------------------------------------------------
    // ...
    //----------------------------------------------------------------------------

    // TODO: Initialize input events system (wl_seat: pointer, keyboard)
    //----------------------------------------------------------------------------
    // ...
    //----------------------------------------------------------------------------

    // Initialize timing system
    //----------------------------------------------------------------------------
    InitTimer();
    //----------------------------------------------------------------------------

    // Initialize storage system
    //----------------------------------------------------------------------------
    CORE.Storage.basePath = GetWorkingDirectory();
    //----------------------------------------------------------------------------

    TRACELOG(LOG_INFO, "PLATFORM: WAYLAND LAYER-SHELL: Initialized successfully");

    return 0;
}

// Close platform
void ClosePlatform(void)
{
    // TODO: Destroy EGL context and surface

    // TODO: Destroy input objects

    CloseLayerSurface();
    CloseWaylandConnection();
}

// Create the primary layer surface from layerConfig and wait for the first configure event
// NOTE: Sizes: 0 on an axis means "stretch between anchors" and requires the surface to be
// anchored to both opposite edges of that axis, otherwise it is a protocol error. In that case
// the InitWindow() size is used as the request and, if it is 0 too, the axis is auto-stretched
static int InitLayerSurface(void)
{
    platform.wlSurface = wl_compositor_create_surface(platform.compositor);
    if (platform.wlSurface == NULL)
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to create wl_surface");
        return -1;
    }

    wl_surface_add_listener(platform.wlSurface, &surfaceListener, NULL);

    // Fractional scale requires viewporter to map the scaled buffer to the logical surface size
    if ((platform.fractionalScaleManager != NULL) && (platform.viewporter != NULL))
    {
        platform.viewport = wp_viewporter_get_viewport(platform.viewporter, platform.wlSurface);
        platform.fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(platform.fractionalScaleManager, platform.wlSurface);
        wp_fractional_scale_v1_add_listener(platform.fractionalScale, &fractionalScaleListener, NULL);
    }

    // Resolve requested output, NULL lets the compositor choose (usually the focused one)
    struct wl_output *output = NULL;
    if (layerConfig.output[0] != '\0')
    {
        output = FindOutputByName(layerConfig.output);
        if (output == NULL) TRACELOG(LOG_WARNING, "LAYER: Output \"%s\" not found, letting the compositor choose", layerConfig.output);
    }

    if ((layerConfig.layer < LAYER_SHELL_BACKGROUND) || (layerConfig.layer > LAYER_SHELL_OVERLAY))
    {
        TRACELOG(LOG_WARNING, "LAYER: Invalid layer %i, using LAYER_SHELL_TOP", layerConfig.layer);
        layerConfig.layer = LAYER_SHELL_TOP;
    }

    platform.layerSurface = zwlr_layer_shell_v1_get_layer_surface(platform.layerShell, platform.wlSurface, output, (uint32_t)layerConfig.layer, layerConfig.nameSpace);
    if (platform.layerSurface == NULL)
    {
        TRACELOG(LOG_WARNING, "LAYER: Failed to create zwlr_layer_surface_v1");
        return -1;
    }

    zwlr_layer_surface_v1_add_listener(platform.layerSurface, &layerSurfaceListener, NULL);

    // Compute requested size: config size wins, InitWindow() size is the fallback hint
    int anchor = layerConfig.anchor & (LAYER_SHELL_ANCHOR_TOP | LAYER_SHELL_ANCHOR_BOTTOM | LAYER_SHELL_ANCHOR_LEFT | LAYER_SHELL_ANCHOR_RIGHT);
    bool stretchX = ((anchor & LAYER_SHELL_ANCHOR_LEFT) && (anchor & LAYER_SHELL_ANCHOR_RIGHT));
    bool stretchY = ((anchor & LAYER_SHELL_ANCHOR_TOP) && (anchor & LAYER_SHELL_ANCHOR_BOTTOM));

    int width = (layerConfig.width > 0)? layerConfig.width : 0;
    int height = (layerConfig.height > 0)? layerConfig.height : 0;

    if ((width == 0) && !stretchX) width = CORE.Window.screen.width;
    if ((height == 0) && !stretchY) height = CORE.Window.screen.height;

    if ((width == 0) && !stretchX)
    {
        TRACELOG(LOG_WARNING, "LAYER: No width provided and surface not anchored left+right, stretching horizontally");
        anchor |= (LAYER_SHELL_ANCHOR_LEFT | LAYER_SHELL_ANCHOR_RIGHT);
    }

    if ((height == 0) && !stretchY)
    {
        TRACELOG(LOG_WARNING, "LAYER: No height provided and surface not anchored top+bottom, stretching vertically");
        anchor |= (LAYER_SHELL_ANCHOR_TOP | LAYER_SHELL_ANCHOR_BOTTOM);
    }

    layerConfig.anchor = anchor;
    layerConfig.width = width;
    layerConfig.height = height;

    int keyboard = layerConfig.keyboard;
    if ((keyboard == LAYER_SHELL_KEYBOARD_ON_DEMAND) && (zwlr_layer_surface_v1_get_version(platform.layerSurface) < 4))
    {
        TRACELOG(LOG_WARNING, "LAYER: Compositor layer-shell version does not support on-demand keyboard, using exclusive");
        keyboard = LAYER_SHELL_KEYBOARD_EXCLUSIVE;
        layerConfig.keyboard = keyboard;
    }

    zwlr_layer_surface_v1_set_anchor(platform.layerSurface, (uint32_t)anchor);
    zwlr_layer_surface_v1_set_size(platform.layerSurface, (uint32_t)width, (uint32_t)height);
    zwlr_layer_surface_v1_set_exclusive_zone(platform.layerSurface, layerConfig.exclusiveZone);
    zwlr_layer_surface_v1_set_margin(platform.layerSurface, layerConfig.marginTop, layerConfig.marginRight, layerConfig.marginBottom, layerConfig.marginLeft);
    zwlr_layer_surface_v1_set_keyboard_interactivity(platform.layerSurface, (uint32_t)keyboard);

    // Until the compositor answers, assume the requested size
    platform.logicalWidth = width;
    platform.logicalHeight = height;

    // Initial commit without a buffer: the compositor answers with a configure event
    wl_surface_commit(platform.wlSurface);

    while (!platform.configured)
    {
        if (wl_display_roundtrip(platform.display) < 0)
        {
            TRACELOG(LOG_WARNING, "LAYER: Display error while waiting for layer surface configure");
            return -1;
        }

        if (CORE.Window.shouldClose)
        {
            TRACELOG(LOG_WARNING, "LAYER: Compositor closed the layer surface before configuring it");
            return -1;
        }
    }

    TRACELOG(LOG_INFO, "LAYER: Surface \"%s\" created on layer %i, anchor 0x%x, exclusive zone %i", layerConfig.nameSpace, layerConfig.layer, anchor, layerConfig.exclusiveZone);
    TRACELOG(LOG_INFO, "LAYER: Configured size: %i x %i (logical), scale %.2f", platform.logicalWidth, platform.logicalHeight, platform.scale);

    return 0;
}

// Destroy the primary layer surface objects
static void CloseLayerSurface(void)
{
    if (platform.fractionalScale != NULL) wp_fractional_scale_v1_destroy(platform.fractionalScale);
    if (platform.viewport != NULL) wp_viewport_destroy(platform.viewport);
    if (platform.layerSurface != NULL) zwlr_layer_surface_v1_destroy(platform.layerSurface);
    if (platform.wlSurface != NULL) wl_surface_destroy(platform.wlSurface);

    platform.fractionalScale = NULL;
    platform.viewport = NULL;
    platform.layerSurface = NULL;
    platform.wlSurface = NULL;
    platform.configured = false;
}

// Recompute buffer size from logical size and scale, update CORE.Window
// NOTE: Follows raylib high-DPI conventions:
//  -> CORE.Window.screen: logical size (what the user draws in)
//  -> CORE.Window.render: physical buffer size (logical*scale)
//  -> CORE.Window.screenScale: MatrixScale(scale, scale, 1) applied to the default framebuffer drawing
static void UpdateSurfaceGeometry(void)
{
    if ((platform.logicalWidth <= 0) || (platform.logicalHeight <= 0)) return;

    // Choose scale source: fractional scale (needs viewporter) > wl_surface.preferred_buffer_scale > output integer scale
    if (platform.viewport == NULL)
    {
        int intScale = 1;
        if (platform.preferredBufferScale > 0) intScale = platform.preferredBufferScale;
        else if ((platform.currentOutput >= 0) && (platform.currentOutput < platform.outputCount)) intScale = platform.outputs[platform.currentOutput].scale;
        platform.scale = (double)intScale;
    }
    else if (platform.scale <= 0.0) platform.scale = 1.0;

    int physicalWidth = (int)(platform.logicalWidth*platform.scale + 0.5);
    int physicalHeight = (int)(platform.logicalHeight*platform.scale + 0.5);

    if (platform.eglWindow != NULL) wl_egl_window_resize(platform.eglWindow, physicalWidth, physicalHeight, 0, 0);

    if (platform.viewport != NULL)
    {
        // Viewport maps the whole (scaled) buffer to the logical surface size
        wp_viewport_set_destination(platform.viewport, platform.logicalWidth, platform.logicalHeight);
    }
    else if (wl_surface_get_version(platform.wlSurface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
    {
        wl_surface_set_buffer_scale(platform.wlSurface, (int32_t)platform.scale);
    }

    bool sizeChanged = ((CORE.Window.render.width != physicalWidth) || (CORE.Window.render.height != physicalHeight) ||
                        (CORE.Window.screen.width != platform.logicalWidth) || (CORE.Window.screen.height != platform.logicalHeight));

    CORE.Window.screen.width = platform.logicalWidth;
    CORE.Window.screen.height = platform.logicalHeight;
    CORE.Window.render.width = physicalWidth;
    CORE.Window.render.height = physicalHeight;
    CORE.Window.currentFbo = CORE.Window.render;
    CORE.Window.screenScale = MatrixScale((float)platform.scale, (float)platform.scale, 1.0f);

    // Viewport can only be reset once the GL context exists (after InitWindow() completes)
    if (CORE.Window.ready)
    {
        SetupViewport(physicalWidth, physicalHeight);
        if (sizeChanged) CORE.Window.resizedLastFrame = true;
    }
}

// Get tracked output index from protocol object
static int GetOutputIndex(struct wl_output *output)
{
    for (int i = 0; i < platform.outputCount; i++)
    {
        if (platform.outputs[i].output == output) return i;
    }

    return -1;
}

// Get output protocol object from connector name
static struct wl_output *FindOutputByName(const char *name)
{
    for (int i = 0; i < platform.outputCount; i++)
    {
        if (strcmp(platform.outputs[i].name, name) == 0) return platform.outputs[i].output;
    }

    return NULL;
}

// Layer surface: compositor assigned a size, must be acknowledged
// NOTE: A 0 dimension means the client is free to choose, the requested size is kept
static void LayerSurfaceConfigureCallback(void *data, struct zwlr_layer_surface_v1 *layerSurface, uint32_t serial, uint32_t width, uint32_t height)
{
    if (width > 0) platform.logicalWidth = (int)width;
    else if (platform.logicalWidth <= 0) platform.logicalWidth = (layerConfig.width > 0)? layerConfig.width : 1;

    if (height > 0) platform.logicalHeight = (int)height;
    else if (platform.logicalHeight <= 0) platform.logicalHeight = (layerConfig.height > 0)? layerConfig.height : 1;

    zwlr_layer_surface_v1_ack_configure(layerSurface, serial);

    platform.configured = true;

    UpdateSurfaceGeometry();
}

// Layer surface: compositor closed the surface (output removed, shell restarted...)
static void LayerSurfaceClosedCallback(void *data, struct zwlr_layer_surface_v1 *layerSurface)
{
    TRACELOG(LOG_INFO, "LAYER: Surface closed by the compositor");
    CORE.Window.shouldClose = true;
}

// Surface: now displayed on an output
static void SurfaceEnterCallback(void *data, struct wl_surface *surface, struct wl_output *output)
{
    int index = GetOutputIndex(output);
    if (index < 0) return;

    platform.currentOutput = index;

    // Scale may differ between outputs when fractional scale is unavailable
    if (platform.viewport == NULL) UpdateSurfaceGeometry();
}

// Surface: no longer displayed on an output
static void SurfaceLeaveCallback(void *data, struct wl_surface *surface, struct wl_output *output)
{
    int index = GetOutputIndex(output);
    if ((index >= 0) && (platform.currentOutput == index)) platform.currentOutput = -1;
}

// Surface: compositor suggested integer buffer scale (wl_surface v6)
static void SurfacePreferredBufferScaleCallback(void *data, struct wl_surface *surface, int32_t factor)
{
    platform.preferredBufferScale = (factor > 0)? factor : 1;

    if (platform.viewport == NULL) UpdateSurfaceGeometry();
}

// Surface: compositor suggested buffer transform (wl_surface v6), not used
static void SurfacePreferredBufferTransformCallback(void *data, struct wl_surface *surface, uint32_t transform)
{
    // Rotated outputs are handled by the compositor, buffer is always kept unrotated
}

// Fractional scale: compositor preferred scale in 1/120 units
static void FractionalScalePreferredScaleCallback(void *data, struct wp_fractional_scale_v1 *fractionalScale, uint32_t scale)
{
    double newScale = (scale > 0)? ((double)scale/120.0) : 1.0;

    if (newScale != platform.scale)
    {
        platform.scale = newScale;
        UpdateSurfaceGeometry();
    }
}

// Connect to the Wayland display and bind required globals
// NOTE: Two roundtrips are required: the first one receives the registry globals,
// the second one receives the initial events of the bound objects (outputs geometry/mode/name)
static int InitWaylandConnection(void)
{
    platform.display = wl_display_connect(NULL);
    if (platform.display == NULL)
    {
        TRACELOG(LOG_WARNING, "WAYLAND: Failed to connect to display (is WAYLAND_DISPLAY set?)");
        return -1;
    }

    platform.registry = wl_display_get_registry(platform.display);
    if (platform.registry == NULL)
    {
        TRACELOG(LOG_WARNING, "WAYLAND: Failed to get registry");
        return -1;
    }

    wl_registry_add_listener(platform.registry, &registryListener, NULL);

    if (wl_display_roundtrip(platform.display) < 0)
    {
        TRACELOG(LOG_WARNING, "WAYLAND: Failed to receive registry globals");
        return -1;
    }

    if (platform.compositor == NULL)
    {
        TRACELOG(LOG_WARNING, "WAYLAND: Compositor does not provide wl_compositor v%i", WL_COMPOSITOR_BIND_VERSION);
        return -1;
    }

    if (platform.layerShell == NULL)
    {
        TRACELOG(LOG_WARNING, "WAYLAND: Compositor does not provide zwlr_layer_shell_v1 (wlr-layer-shell is required, e.g. Hyprland, sway, river)");
        return -1;
    }

    if (platform.seat == NULL) TRACELOG(LOG_WARNING, "WAYLAND: Compositor does not provide wl_seat, no input available");
    if (platform.wmBase == NULL) TRACELOG(LOG_INFO, "WAYLAND: xdg_wm_base not available, layer-shell popups disabled");
    if ((platform.fractionalScaleManager == NULL) || (platform.viewporter == NULL))
    {
        TRACELOG(LOG_INFO, "WAYLAND: Fractional scale not available, using integer output scale");
    }

    // Receive initial events of bound globals (output properties, seat capabilities)
    if (wl_display_roundtrip(platform.display) < 0)
    {
        TRACELOG(LOG_WARNING, "WAYLAND: Failed to receive initial globals events");
        return -1;
    }

    if (platform.outputCount == 0) TRACELOG(LOG_WARNING, "WAYLAND: No outputs advertised by the compositor");

    for (int i = 0; i < platform.outputCount; i++)
    {
        TRACELOG(LOG_INFO, "WAYLAND: Output %i: %s (%s) %ix%i@%iHz at (%i,%i), scale %i, %ix%i mm", i,
            (platform.outputs[i].name[0] != '\0')? platform.outputs[i].name : "unnamed", platform.outputs[i].description,
            platform.outputs[i].width, platform.outputs[i].height, (platform.outputs[i].refreshRate + 500)/1000,
            platform.outputs[i].x, platform.outputs[i].y, platform.outputs[i].scale,
            platform.outputs[i].physicalWidth, platform.outputs[i].physicalHeight);
    }

    // Display size defaults to the first output current mode
    if (platform.outputCount > 0)
    {
        CORE.Window.display.width = platform.outputs[0].width;
        CORE.Window.display.height = platform.outputs[0].height;
    }

    TRACELOG(LOG_INFO, "WAYLAND: Connected to display, %i output(s) available", platform.outputCount);

    return 0;
}

// Release globals and disconnect from the Wayland display
// NOTE: Objects are destroyed in reverse order of creation
static void CloseWaylandConnection(void)
{
    for (int i = 0; i < platform.outputCount; i++)
    {
        if (platform.outputs[i].output != NULL) wl_output_destroy(platform.outputs[i].output);
        platform.outputs[i].output = NULL;
    }
    platform.outputCount = 0;

    if (platform.viewporter != NULL) wp_viewporter_destroy(platform.viewporter);
    if (platform.fractionalScaleManager != NULL) wp_fractional_scale_manager_v1_destroy(platform.fractionalScaleManager);
    if (platform.wmBase != NULL) xdg_wm_base_destroy(platform.wmBase);
    if (platform.seat != NULL) wl_seat_destroy(platform.seat);
    if (platform.layerShell != NULL) zwlr_layer_shell_v1_destroy(platform.layerShell);
    if (platform.compositor != NULL) wl_compositor_destroy(platform.compositor);
    if (platform.registry != NULL) wl_registry_destroy(platform.registry);

    platform.viewporter = NULL;
    platform.fractionalScaleManager = NULL;
    platform.wmBase = NULL;
    platform.seat = NULL;
    platform.layerShell = NULL;
    platform.compositor = NULL;
    platform.registry = NULL;

    if (platform.display != NULL)
    {
        wl_display_flush(platform.display);
        wl_display_disconnect(platform.display);
        platform.display = NULL;
    }
}

// Registry: new global advertised
static void RegistryGlobalCallback(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        // wl_surface v6 provides preferred_buffer_scale, bind the highest supported version
        uint32_t bindVersion = (version < 6)? version : 6;
        if (bindVersion >= WL_COMPOSITOR_BIND_VERSION) platform.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, bindVersion);
        else TRACELOG(LOG_WARNING, "WAYLAND: wl_compositor v%u too old, v%i required", version, WL_COMPOSITOR_BIND_VERSION);
    }
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
    {
        uint32_t bindVersion = (version < WL_LAYER_SHELL_BIND_VERSION)? version : WL_LAYER_SHELL_BIND_VERSION;
        platform.layerShell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, bindVersion);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        // Only the first seat is used
        if (platform.seat == NULL)
        {
            uint32_t bindVersion = (version < WL_SEAT_BIND_VERSION)? version : WL_SEAT_BIND_VERSION;
            platform.seat = wl_registry_bind(registry, name, &wl_seat_interface, bindVersion);
        }
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        if (platform.outputCount >= MAX_WAYLAND_OUTPUTS)
        {
            TRACELOG(LOG_WARNING, "WAYLAND: Too many outputs, ignoring output global %u (max: %i)", name, MAX_WAYLAND_OUTPUTS);
            return;
        }

        uint32_t bindVersion = (version < WL_OUTPUT_BIND_VERSION)? version : WL_OUTPUT_BIND_VERSION;
        WaylandOutput *out = &platform.outputs[platform.outputCount];
        memset(out, 0, sizeof(WaylandOutput));
        out->globalName = name;
        out->scale = 1;
        out->output = wl_registry_bind(registry, name, &wl_output_interface, bindVersion);
        wl_output_add_listener(out->output, &outputListener, out);
        platform.outputCount++;
    }
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        platform.wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(platform.wmBase, &wmBaseListener, NULL);
    }
    else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
    {
        platform.fractionalScaleManager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    }
    else if (strcmp(interface, wp_viewporter_interface.name) == 0)
    {
        platform.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    }
}

// Registry: global removed (typically an output being unplugged)
static void RegistryGlobalRemoveCallback(void *data, struct wl_registry *registry, uint32_t name)
{
    for (int i = 0; i < platform.outputCount; i++)
    {
        if (platform.outputs[i].globalName == name)
        {
            TRACELOG(LOG_INFO, "WAYLAND: Output removed: %s", (platform.outputs[i].name[0] != '\0')? platform.outputs[i].name : "unnamed");

            wl_output_destroy(platform.outputs[i].output);

            // Keep the array compact, preserving order of remaining outputs
            for (int j = i; j < (platform.outputCount - 1); j++) platform.outputs[j] = platform.outputs[j + 1];
            platform.outputCount--;
            memset(&platform.outputs[platform.outputCount], 0, sizeof(WaylandOutput));

            if (platform.currentOutput == i) platform.currentOutput = -1;
            else if (platform.currentOutput > i) platform.currentOutput--;

            return;
        }
    }
}

// Output: geometry (position, physical size, make/model)
static void OutputGeometryCallback(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t physicalWidth, int32_t physicalHeight, int32_t subpixel, const char *make, const char *model, int32_t transform)
{
    WaylandOutput *out = (WaylandOutput *)data;

    out->x = x;
    out->y = y;
    out->physicalWidth = physicalWidth;
    out->physicalHeight = physicalHeight;

    // Description event (v4) overrides this when available
    if (out->description[0] == '\0')
    {
        snprintf(out->description, sizeof(out->description), "%s %s", (make != NULL)? make : "", (model != NULL)? model : "");
    }
}

// Output: mode (only the current mode is tracked)
static void OutputModeCallback(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    WaylandOutput *out = (WaylandOutput *)data;

    if (flags & WL_OUTPUT_MODE_CURRENT)
    {
        out->width = width;
        out->height = height;
        out->refreshRate = refresh;
    }
}

// Output: all pending properties sent
static void OutputDoneCallback(void *data, struct wl_output *output)
{
    WaylandOutput *out = (WaylandOutput *)data;

    out->done = true;

    // Keep display size in sync with the first output
    if ((platform.outputCount > 0) && (out == &platform.outputs[0]))
    {
        CORE.Window.display.width = out->width;
        CORE.Window.display.height = out->height;
    }
}

// Output: integer scale factor
static void OutputScaleCallback(void *data, struct wl_output *output, int32_t factor)
{
    WaylandOutput *out = (WaylandOutput *)data;

    out->scale = (factor > 0)? factor : 1;
}

// Output: connector name (wl_output v4)
static void OutputNameCallback(void *data, struct wl_output *output, const char *name)
{
    WaylandOutput *out = (WaylandOutput *)data;

    if (name != NULL) strncpy(out->name, name, LAYER_OUTPUT_NAME_LENGTH - 1);
}

// Output: human readable description (wl_output v4)
static void OutputDescriptionCallback(void *data, struct wl_output *output, const char *description)
{
    WaylandOutput *out = (WaylandOutput *)data;

    if (description != NULL) strncpy(out->description, description, sizeof(out->description) - 1);
}

// xdg_wm_base: compositor liveness check, must be answered or the client gets killed
static void WmBasePingCallback(void *data, struct xdg_wm_base *wmBase, uint32_t serial)
{
    xdg_wm_base_pong(wmBase, serial);
}

// EOF
