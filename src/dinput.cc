#include "dinput.h"

#include <cmath>

#include "display_scaler.h"

namespace fallout {

static int gMouseWheelDeltaX = 0;
static int gMouseWheelDeltaY = 0;
static double gMouseDeltaAccumulatorX = 0.0;
static double gMouseDeltaAccumulatorY = 0.0;

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
    return true;
}

// 0x4E0514
bool mouseDeviceUnacquire()
{
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

    Uint32 buttons = SDL_GetRelativeMouseState(&(mouseState->x), &(mouseState->y));

    double inverseScale = displayScalerGetInverseScale();
    double logicalDeltaX = gMouseDeltaAccumulatorX + inverseScale * mouseState->x;
    double logicalDeltaY = gMouseDeltaAccumulatorY + inverseScale * mouseState->y;

    int convertedX = static_cast<int>(std::round(logicalDeltaX));
    int convertedY = static_cast<int>(std::round(logicalDeltaY));

    gMouseDeltaAccumulatorX = logicalDeltaX - convertedX;
    gMouseDeltaAccumulatorY = logicalDeltaY - convertedY;

    mouseState->x = convertedX;
    mouseState->y = convertedY;
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
    return SDL_SetRelativeMouseMode(SDL_TRUE) == 0;
}

// 0x4E078C
void mouseDeviceFree()
{
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
    // Mouse movement and buttons are accumulated in SDL itself and will be
    // processed later in `mouseDeviceGetData` via `SDL_GetRelativeMouseState`.

    if (event->type == SDL_MOUSEWHEEL) {
        gMouseWheelDeltaX += event->wheel.x;
        gMouseWheelDeltaY += event->wheel.y;
    }
}

} // namespace fallout
