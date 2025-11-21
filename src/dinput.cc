#include "dinput.h"

#include <algorithm>
#include <cmath>

#include "display_scaler.h"
#include "svga.h"

namespace fallout {

static int gMouseWheelDeltaX = 0;
static int gMouseWheelDeltaY = 0;
static bool gMouseHasPosition = false;
static double gMouseLogicalExactX = 0.0;
static double gMouseLogicalExactY = 0.0;
static double gMouseLogicalRemainderX = 0.0;
static double gMouseLogicalRemainderY = 0.0;

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

    int outputW = 0;
    int outputH = 0;
    if (gSdlRenderer != nullptr) {
        SDL_GetRendererOutputSize(gSdlRenderer, &outputW, &outputH);
    }

    if (outputW == 0 || outputH == 0) {
        // Fallback to window size if renderer not available.
        if (gSdlWindow != nullptr) {
            SDL_GetWindowSize(gSdlWindow, &outputW, &outputH);
        }
    }

    PhysicalSpace physicalSpace = displayScalerGetPhysicalSpace();

    double scaleX = 1.0;
    double scaleY = 1.0;
    if (outputW > 0) {
        scaleX = static_cast<double>(physicalSpace.width) / static_cast<double>(outputW);
    }
    if (outputH > 0) {
        scaleY = static_cast<double>(physicalSpace.height) / static_cast<double>(outputH);
    }

    int physicalX = static_cast<int>(std::round(windowX * scaleX));
    int physicalY = static_cast<int>(std::round(windowY * scaleY));

    physicalX = std::clamp(physicalX, 0, std::max(0, physicalSpace.width - 1));
    physicalY = std::clamp(physicalY, 0, std::max(0, physicalSpace.height - 1));

    const Rect& viewport = displayScalerGetPhysicalViewport();
    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    double inverseScale = displayScalerGetInverseScale();

    double localX = static_cast<double>(physicalX - viewport.left);
    double localY = static_cast<double>(physicalY - viewport.top);

    double logicalExactX = localX * inverseScale;
    double logicalExactY = localY * inverseScale;

    double logicalMaxX = static_cast<double>(logicalSpace.width - 1);
    double logicalMaxY = static_cast<double>(logicalSpace.height - 1);

    logicalExactX = std::clamp(logicalExactX, 0.0, logicalMaxX);
    logicalExactY = std::clamp(logicalExactY, 0.0, logicalMaxY);

    if (!gMouseHasPosition) {
        gMouseLogicalExactX = logicalExactX;
        gMouseLogicalExactY = logicalExactY;
        gMouseLogicalRemainderX = 0.0;
        gMouseLogicalRemainderY = 0.0;
        gMouseHasPosition = true;
        mouseState->x = 0;
        mouseState->y = 0;
    } else {
        double deltaXExact = logicalExactX - gMouseLogicalExactX + gMouseLogicalRemainderX;
        double deltaYExact = logicalExactY - gMouseLogicalExactY + gMouseLogicalRemainderY;

        int deltaX = static_cast<int>(std::round(deltaXExact));
        int deltaY = static_cast<int>(std::round(deltaYExact));

        gMouseLogicalRemainderX = deltaXExact - deltaX;
        gMouseLogicalRemainderY = deltaYExact - deltaY;

        gMouseLogicalExactX = logicalExactX;
        gMouseLogicalExactY = logicalExactY;

        mouseState->x = deltaX;
        mouseState->y = deltaY;
    }

    mouseState->buttons[0] = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    mouseState->buttons[1] = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    mouseState->wheelX = gMouseWheelDeltaX;
    mouseState->wheelY = gMouseWheelDeltaY;

    gMouseWheelDeltaX = 0;
    gMouseWheelDeltaY = 0;

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

void handleMouseEvent(SDL_Event* event)
{
    // Mouse wheel events are accumulated here; absolute position is read in
    // `mouseDeviceGetData` via `SDL_GetMouseState` each frame.

    if (event->type == SDL_MOUSEWHEEL) {
        gMouseWheelDeltaX += event->wheel.x;
        gMouseWheelDeltaY += event->wheel.y;
    }
}

} // namespace fallout
