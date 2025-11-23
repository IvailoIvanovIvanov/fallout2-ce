#include "virtual_input.h"

#include <algorithm>
#include <cmath>

#include "display_scaler.h"
#include "settings.h"
#include "svga.h"

namespace fallout {
namespace {

struct MouseWindowSample {
    bool valid = false;
    int x = 0;
    int y = 0;
};

struct OverlayState {
    bool hasSample = false;
    VirtualMouseMappingSample sample = {};
};

MouseWindowSample gMouseWindowSample;
OverlayState gOverlayState;
int gMouseWheelDeltaX = 0;
int gMouseWheelDeltaY = 0;

constexpr int kCrossHalfSize = 8;

static int clampToPhysicalX(int x)
{
    const int width = std::max(1, screenGetPhysicalWidth());
    return std::clamp(x, 0, width - 1);
}

static int clampToPhysicalY(int y)
{
    const int height = std::max(1, screenGetPhysicalHeight());
    return std::clamp(y, 0, height - 1);
}

static SDL_Point clampPointToPhysical(SDL_Point point)
{
    SDL_Point clamped;
    clamped.x = clampToPhysicalX(point.x);
    clamped.y = clampToPhysicalY(point.y);
    return clamped;
}

static void drawCross(SDL_Renderer* renderer, const SDL_Point& point, const SDL_Color& color)
{
    if (renderer == nullptr) {
        return;
    }

    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    const SDL_Point center = clampPointToPhysical(point);
    const int left = clampToPhysicalX(center.x - kCrossHalfSize);
    const int right = clampToPhysicalX(center.x + kCrossHalfSize);
    const int top = clampToPhysicalY(center.y - kCrossHalfSize);
    const int bottom = clampToPhysicalY(center.y + kCrossHalfSize);

    SDL_RenderDrawLine(renderer, left, center.y, right, center.y);
    SDL_RenderDrawLine(renderer, center.x, top, center.x, bottom);
}

static void drawViewportOutline(SDL_Renderer* renderer, const Rect& viewport)
{
    if (renderer == nullptr) {
        return;
    }

    SDL_Rect rect;
    rect.x = viewport.left;
    rect.y = viewport.top;
    rect.w = rectGetWidth(&viewport);
    rect.h = rectGetHeight(&viewport);

    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 120);
    SDL_RenderDrawRect(renderer, &rect);
}

} // namespace

void virtualInputReset()
{
    gMouseWindowSample = {};
    gOverlayState = {};
    gMouseWheelDeltaX = 0;
    gMouseWheelDeltaY = 0;
}

void virtualInputCaptureEvent(const SDL_Event* event)
{
    if (event == nullptr) {
        return;
    }

    switch (event->type) {
    case SDL_MOUSEMOTION:
        gMouseWindowSample.valid = true;
        gMouseWindowSample.x = event->motion.x;
        gMouseWindowSample.y = event->motion.y;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        gMouseWindowSample.valid = true;
        gMouseWindowSample.x = event->button.x;
        gMouseWindowSample.y = event->button.y;
        break;
    case SDL_MOUSEWHEEL:
        gMouseWheelDeltaX += event->wheel.x;
        gMouseWheelDeltaY += event->wheel.y;
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        if (gSdlWindow != nullptr) {
            int windowWidth = 0;
            int windowHeight = 0;
            SDL_GetWindowSize(gSdlWindow, &windowWidth, &windowHeight);
            gMouseWindowSample.valid = true;
            gMouseWindowSample.x = static_cast<int>(std::round(event->tfinger.x * windowWidth));
            gMouseWindowSample.y = static_cast<int>(std::round(event->tfinger.y * windowHeight));
        }
        break;
    default:
        break;
    }
}

bool virtualInputPeekMouseWindowPoint(int* windowX, int* windowY)
{
    if (!gMouseWindowSample.valid) {
        return false;
    }

    if (windowX != nullptr) {
        *windowX = gMouseWindowSample.x;
    }

    if (windowY != nullptr) {
        *windowY = gMouseWindowSample.y;
    }

    return true;
}

void virtualInputConsumeWheelDeltas(int* wheelX, int* wheelY)
{
    if (wheelX != nullptr) {
        *wheelX = gMouseWheelDeltaX;
    }

    if (wheelY != nullptr) {
        *wheelY = gMouseWheelDeltaY;
    }

    gMouseWheelDeltaX = 0;
    gMouseWheelDeltaY = 0;
}

void virtualInputPublishMouseMapping(const VirtualMouseMappingSample& sample)
{
    gOverlayState.sample = sample;
    gOverlayState.hasSample = true;
}

void virtualInputRenderOverlay(SDL_Renderer* renderer)
{
    if (renderer == nullptr) {
        return;
    }

    if (!settings.debug.input_overlay) {
        return;
    }

    if (!gOverlayState.hasSample) {
        return;
    }

    const VirtualMouseMappingSample& sample = gOverlayState.sample;

    SDL_BlendMode previousMode;
    SDL_GetRenderDrawBlendMode(renderer, &previousMode);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    drawViewportOutline(renderer, sample.viewport);

    SDL_Point physicalPoint = { sample.physicalX, sample.physicalY };
    SDL_Point physicalClamped = clampPointToPhysical(physicalPoint);
    SDL_Color physicalColor = { 255, 80, 80, 200 };
    drawCross(renderer, physicalClamped, physicalColor);

    Point logicalPoint;
    logicalPoint.x = static_cast<int>(std::round(sample.virtualExactX));
    logicalPoint.y = static_cast<int>(std::round(sample.virtualExactY));
    Point projected = displayScalerLogicalToPhysical(logicalPoint);
    SDL_Point virtualPoint = { projected.x, projected.y };
    SDL_Color virtualColor = { 80, 255, 160, 200 };
    drawCross(renderer, virtualPoint, virtualColor);

    SDL_SetRenderDrawBlendMode(renderer, previousMode);
}

} // namespace fallout
