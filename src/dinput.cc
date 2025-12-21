#include "dinput.h"

#include <algorithm>
#include <cmath>

#include "renderer/display_scaler.h"
#include "diagnostics.h"
#include "svga.h"

namespace fallout {

static bool gMouseHasPosition = false;
static double gMouseLogicalExactX = 0.0;
static double gMouseLogicalExactY = 0.0;
static double gMouseLogicalRemainderX = 0.0;
static double gMouseLogicalRemainderY = 0.0;
static int gWheelDeltaX = 0;
static int gWheelDeltaY = 0;
static bool gMouseButtonPressedSinceLastPoll[2] = { false, false };

namespace {

struct MouseMetricsSnapshot {
    int windowWidth;
    int windowHeight;
    int drawableWidth;
    int drawableHeight;
    int physicalWidth;
    int physicalHeight;
};

struct MouseClampSnapshot {
    bool lowX;
    bool highX;
    bool lowY;
    bool highY;
};

MouseMetricsSnapshot gLastMouseMetrics = {};
bool gHasMouseMetrics = false;
MouseClampSnapshot gLastClampSnapshot = {};
bool gHasClampSnapshot = false;

void logMouseMetricsChange(const MouseMetricsSnapshot& snapshot)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        return;
    }

    if (gHasMouseMetrics && gLastMouseMetrics.windowWidth == snapshot.windowWidth && gLastMouseMetrics.windowHeight == snapshot.windowHeight && gLastMouseMetrics.drawableWidth == snapshot.drawableWidth && gLastMouseMetrics.drawableHeight == snapshot.drawableHeight && gLastMouseMetrics.physicalWidth == snapshot.physicalWidth && gLastMouseMetrics.physicalHeight == snapshot.physicalHeight) {
        return;
    }

    gLastMouseMetrics = snapshot;
    gHasMouseMetrics = true;

    diagnosticsLog(
        DiagnosticsLevel::Info,
        "MOUSE",
        "metrics window=%dx%d drawable=%dx%d physical=%dx%d",
        snapshot.windowWidth,
        snapshot.windowHeight,
        snapshot.drawableWidth,
        snapshot.drawableHeight,
        snapshot.physicalWidth,
        snapshot.physicalHeight);
}

void logMouseClampChange(const MouseClampSnapshot& snapshot, double logicalX, double logicalY)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        return;
    }

    if (gHasClampSnapshot && gLastClampSnapshot.lowX == snapshot.lowX && gLastClampSnapshot.highX == snapshot.highX && gLastClampSnapshot.lowY == snapshot.lowY && gLastClampSnapshot.highY == snapshot.highY) {
        return;
    }

    gLastClampSnapshot = snapshot;
    gHasClampSnapshot = true;

    diagnosticsLog(
        DiagnosticsLevel::Info,
        "MOUSE",
        "clamp lowX=%d highX=%d lowY=%d highY=%d logical=(%.2f,%.2f)",
        snapshot.lowX ? 1 : 0,
        snapshot.highX ? 1 : 0,
        snapshot.lowY ? 1 : 0,
        snapshot.highY ? 1 : 0,
        logicalX,
        logicalY);
}

void logMousePrimed(double logicalX, double logicalY, int physicalX, int physicalY)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        return;
    }

    diagnosticsLog(
        DiagnosticsLevel::Info,
        "MOUSE",
        "primed logical=(%.2f,%.2f) physical=(%d,%d)",
        logicalX,
        logicalY,
        physicalX,
        physicalY);
}

} // namespace

// 0x4E0400
bool directInputInit()
{
    if (!mouseDeviceInit()) {
        goto err;
    }

    if (!keyboardDeviceInit()) {
        goto err;
    }

    return true;

err:

    directInputFree();

    return false;
}

// 0x4E0478
void directInputFree()
{
}

// 0x4E04E8
bool mouseDeviceAcquire()
{
    gMouseHasPosition = false;
    gMouseLogicalExactX = 0.0;
    gMouseLogicalExactY = 0.0;
    gMouseLogicalRemainderX = 0.0;
    gMouseLogicalRemainderY = 0.0;
    gHasMouseMetrics = false;
    gHasClampSnapshot = false;
    return true;
}

// 0x4E0514
bool mouseDeviceUnacquire()
{
    gMouseHasPosition = false;
    gMouseLogicalExactX = 0.0;
    gMouseLogicalExactY = 0.0;
    gMouseLogicalRemainderX = 0.0;
    gMouseLogicalRemainderY = 0.0;
    gHasMouseMetrics = false;
    gHasClampSnapshot = false;
    return true;
}

// 0x4E053C
bool mouseDeviceGetData(MouseData* mouseState)
{
    // CE: This function is sometimes called outside loops calling `get_input`
    // and subsequently `GNW95_process_message`, so mouse events might not be
    // handled by SDL yet.
    //
    // TODO: Move mouse events processing into `GNW95_process_message` and
    // update mouse position manually.
    SDL_PumpEvents();

    int windowX;
    int windowY;
    Uint32 buttons = SDL_GetMouseState(&windowX, &windowY);

    int windowWidth = 0;
    int windowHeight = 0;
    if (gSdlWindow != nullptr) {
        SDL_GetWindowSize(gSdlWindow, &windowWidth, &windowHeight);
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    if (gSdlRenderer != nullptr) {
        SDL_GetRendererOutputSize(gSdlRenderer, &drawableWidth, &drawableHeight);
    }

    PhysicalSpace physicalSpace = displayScalerGetPhysicalSpace();

    if (drawableWidth <= 0 || drawableHeight <= 0) {
        drawableWidth = windowWidth;
        drawableHeight = windowHeight;
    }

    if (drawableWidth <= 0 || drawableHeight <= 0) {
        drawableWidth = physicalSpace.width;
        drawableHeight = physicalSpace.height;
    }

    if (windowWidth <= 0) {
        windowWidth = drawableWidth;
    }

    if (windowHeight <= 0) {
        windowHeight = drawableHeight;
    }

    MouseMetricsSnapshot metricsSnapshot = {
        windowWidth,
        windowHeight,
        drawableWidth,
        drawableHeight,
        physicalSpace.width,
        physicalSpace.height,
    };
    logMouseMetricsChange(metricsSnapshot);

    const int clampedWindowX = (windowWidth > 0) ? std::clamp(windowX, 0, windowWidth) : 0;
    const int clampedWindowY = (windowHeight > 0) ? std::clamp(windowY, 0, windowHeight) : 0;

    double pixelRatioX = 1.0;
    double pixelRatioY = 1.0;

    if (windowWidth > 0) {
        pixelRatioX = static_cast<double>(drawableWidth) / static_cast<double>(windowWidth);
    }

    if (windowHeight > 0) {
        pixelRatioY = static_cast<double>(drawableHeight) / static_cast<double>(windowHeight);
    }

    int physicalX = static_cast<int>(std::round(clampedWindowX * pixelRatioX));
    int physicalY = static_cast<int>(std::round(clampedWindowY * pixelRatioY));

    physicalX = std::clamp(physicalX, 0, std::max(0, physicalSpace.width - 1));
    physicalY = std::clamp(physicalY, 0, std::max(0, physicalSpace.height - 1));

    DisplayScalerVirtualMapping mapping = displayScalerMapPointToVirtual(physicalX, physicalY);
    double logicalExactX = mapping.exactX;
    double logicalExactY = mapping.exactY;

    if (!gMouseHasPosition) {
        gMouseLogicalExactX = logicalExactX;
        gMouseLogicalExactY = logicalExactY;
        gMouseLogicalRemainderX = 0.0;
        gMouseLogicalRemainderY = 0.0;
        gMouseHasPosition = true;
        mouseState->x = 0;
        mouseState->y = 0;
        logMousePrimed(logicalExactX, logicalExactY, physicalX, physicalY);
    } else {
        if (mapping.clampedLowX || mapping.clampedHighX) {
            gMouseLogicalRemainderX = 0.0;
        }

        if (mapping.clampedLowY || mapping.clampedHighY) {
            gMouseLogicalRemainderY = 0.0;
        }

        double deltaXExact = logicalExactX - gMouseLogicalExactX + gMouseLogicalRemainderX;
        double deltaYExact = logicalExactY - gMouseLogicalExactY + gMouseLogicalRemainderY;

        int deltaX = static_cast<int>(std::round(deltaXExact));
        int deltaY = static_cast<int>(std::round(deltaYExact));

        gMouseLogicalRemainderX = (mapping.clampedLowX || mapping.clampedHighX) ? 0.0 : (deltaXExact - deltaX);
        gMouseLogicalRemainderY = (mapping.clampedLowY || mapping.clampedHighY) ? 0.0 : (deltaYExact - deltaY);

        gMouseLogicalExactX = logicalExactX;
        gMouseLogicalExactY = logicalExactY;

        mouseState->x = deltaX;
        mouseState->y = deltaY;
    }

    MouseClampSnapshot clampSnapshot = {
        mapping.clampedLowX,
        mapping.clampedHighX,
        mapping.clampedLowY,
        mapping.clampedHighY,
    };
    logMouseClampChange(clampSnapshot, logicalExactX, logicalExactY);

    mouseState->buttons[0] = ((buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0) || gMouseButtonPressedSinceLastPoll[0];
    mouseState->buttons[1] = ((buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0) || gMouseButtonPressedSinceLastPoll[1];

    gMouseButtonPressedSinceLastPoll[0] = false;
    gMouseButtonPressedSinceLastPoll[1] = false;

    mouseState->wheelX = gWheelDeltaX;
    mouseState->wheelY = gWheelDeltaY;
    gWheelDeltaX = 0;
    gWheelDeltaY = 0;

    return true;
}

// 0x4E05A8
bool keyboardDeviceAcquire()
{
    return true;
}

// 0x4E05D4
bool keyboardDeviceUnacquire()
{
    return true;
}

// 0x4E05FC
bool keyboardDeviceReset()
{
    SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT);
    return true;
}

// 0x4E0650
bool keyboardDeviceGetData(KeyboardData* keyboardData)
{
    return true;
}

// 0x4E070C
bool mouseDeviceInit()
{
    if (SDL_SetRelativeMouseMode(SDL_FALSE) != 0) {
        return false;
    }

    SDL_ShowCursor(SDL_DISABLE);

    if (gSdlWindow != nullptr) {
        SDL_SetWindowGrab(gSdlWindow, SDL_TRUE);
    }

    gMouseHasPosition = false;

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        int windowWidth = 0;
        int windowHeight = 0;
        if (gSdlWindow != nullptr) {
            SDL_GetWindowSize(gSdlWindow, &windowWidth, &windowHeight);
        }
        diagnosticsLog(
            DiagnosticsLevel::Info,
            "MOUSE",
            "mouseDeviceInit window=%dx%d grabbed=%d",
            windowWidth,
            windowHeight,
            gSdlWindow != nullptr ? 1 : 0);
    }

    return true;
}

// 0x4E078C
void mouseDeviceFree()
{
    SDL_ShowCursor(SDL_ENABLE);

    if (gSdlWindow != nullptr) {
        SDL_SetWindowGrab(gSdlWindow, SDL_FALSE);
    }

    gMouseHasPosition = false;

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(DiagnosticsLevel::Info, "MOUSE", "mouseDeviceFree");
    }
}

void mouseDeviceAccumulateWheelDelta(int x, int y)
{
    gWheelDeltaX += x;
    gWheelDeltaY += y;
}

void mouseDeviceHandleEvent(const SDL_Event* event)
{
    if (event->type == SDL_MOUSEBUTTONDOWN) {
        if (event->button.button == SDL_BUTTON_LEFT) {
            gMouseButtonPressedSinceLastPoll[0] = true;
        } else if (event->button.button == SDL_BUTTON_RIGHT) {
            gMouseButtonPressedSinceLastPoll[1] = true;
        }
    }
}

// 0x4E07B8
bool keyboardDeviceInit()
{
    return true;
}

// 0x4E0874
void keyboardDeviceFree()
{
}

} // namespace fallout
