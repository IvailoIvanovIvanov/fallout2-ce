#include "svga.h"

#include <algorithm>
#include <limits.h>
#include <string.h>

#include <SDL.h>

#include "color.h"
#include "config.h"
#include "diagnostics.h"
#include "display_scaler.h"
#include "draw.h"
#include "geometry.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

static bool createRenderer();
static void destroyRenderer();
static void syncPhysicalSizeWithRenderer();
static bool rectEquals(const Rect& a, const Rect& b);
static void logViewportIfChanged(const Rect& viewport);

static Rect gLastRenderViewport = { 0, 0, -1, -1 };
static bool gHasRenderViewport = false;

// Legacy screen rect maintained for existing code. Tracks logical bounds.
Rect _scr_size;

// 0x6ACA18
void (*_scr_blit)(unsigned char* src, int src_pitch, int a3, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y) = _GNW95_ShowRect;

// 0x6ACA1C
void (*_zero_mem)() = nullptr;

SDL_Window* gSdlWindow = nullptr;
SDL_Surface* gSdlSurface = nullptr;
SDL_Renderer* gSdlRenderer = nullptr;
SDL_Texture* gSdlTexture = nullptr;
SDL_Surface* gSdlTextureSurface = nullptr;

static bool gTrueColorRendererRequested = false;
static bool gTrueColorRendererActive = false;

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

void blitIndexedBufferToTextureSurface(const unsigned char* src,
    int srcPitch,
    int width,
    int height,
    int destX,
    int destY);

// 0x4CAD08
int _init_mode_320_200()
{
    return _GNW95_init_mode_ex(320, 200, 8);
}

// 0x4CAD40
int _init_mode_320_400()
{
    return _GNW95_init_mode_ex(320, 400, 8);
}

// 0x4CAD5C
int _init_mode_640_480_16()
{
    return -1;
}

// 0x4CAD64
int _init_mode_640_480()
{
    return _init_vesa_mode(640, 480);
}

// 0x4CAD94
int _init_mode_640_400()
{
    return _init_vesa_mode(640, 400);
}

// 0x4CADA8
int _init_mode_800_600()
{
    return _init_vesa_mode(800, 600);
}

// 0x4CADBC
int _init_mode_1024_768()
{
    return _init_vesa_mode(1024, 768);
}

// 0x4CADD0
int _init_mode_1280_1024()
{
    return _init_vesa_mode(1280, 1024);
}

// 0x4CADF8
void _get_start_mode_()
{
}

// 0x4CADFC
void _zero_vid_mem()
{
    if (_zero_mem) {
        _zero_mem();
    }
}

void setTrueColorRendererRequested(bool enabled)
{
    gTrueColorRendererRequested = enabled;
}

bool isTrueColorRendererActive()
{
    return gTrueColorRendererActive;
}

// 0x4CAE1C
int _GNW95_init_mode_ex(int width, int height, int bpp)
{
    bool fullscreen = true;
    int scale = 1;
    int logicalWidth = width;
    int logicalHeight = height;
    bool integerScaling = false;
    bool preserveOriginalLogicalSize = false;
    int logicalScale = 1;
    bool logicalWidthExplicit = false;
    bool logicalHeightExplicit = false;
    bool diagnosticsEnabled = false;
    DiagnosticsLevel diagnosticsLevel = DiagnosticsLevel::Info;
    char* diagnosticsLogFileValue = nullptr;

    Config resolutionConfig;
    if (configInit(&resolutionConfig)) {
        if (configRead(&resolutionConfig, "f2_res.ini", false)) {
            int screenWidth;
            if (configGetInt(&resolutionConfig, "MAIN", "SCR_WIDTH", &screenWidth)) {
                width = screenWidth;
            }

            int screenHeight;
            if (configGetInt(&resolutionConfig, "MAIN", "SCR_HEIGHT", &screenHeight)) {
                height = screenHeight;
            }

            bool windowed;
            if (configGetBool(&resolutionConfig, "MAIN", "WINDOWED", &windowed)) {
                fullscreen = !windowed;
            }

            int scaleValue;
            if (configGetInt(&resolutionConfig, "MAIN", "SCALE_2X", &scaleValue)) {
                scale = scaleValue + 1; // 0 = 1x, 1 = 2x
                // Only allow scaling if resulting game resolution is >= 640x480
                if ((width / scale) < 640 || (height / scale) < 480) {
                    scale = 1;
                } else {
                    width /= scale;
                    height /= scale;
                }
            }

            configGetBool(&resolutionConfig, "IFACE", "IFACE_BAR_MODE", &gInterfaceBarMode);
            configGetInt(&resolutionConfig, "IFACE", "IFACE_BAR_WIDTH", &gInterfaceBarWidth);
            configGetInt(&resolutionConfig, "IFACE", "IFACE_BAR_SIDE_ART", &gInterfaceSidePanelsImageId);
            configGetBool(&resolutionConfig, "IFACE", "IFACE_BAR_SIDES_ORI", &gInterfaceSidePanelsExtendFromScreenEdge);

            configGetBool(&resolutionConfig, "SCALER", "PRESERVE_ORIGINAL_LOGICAL_SIZE", &preserveOriginalLogicalSize);
            if (preserveOriginalLogicalSize) {
                logicalWidth = displayScalerGetDefaultLogicalWidth();
                logicalHeight = displayScalerGetDefaultLogicalHeight();
            }

            int logicalScaleValue;
            if (configGetInt(&resolutionConfig, "SCALER", "LOGICAL_SCALE", &logicalScaleValue)) {
                logicalScale = std::max(1, logicalScaleValue);
            }

            int logicalWidthOverride;
            if (configGetInt(&resolutionConfig, "SCALER", "LOGICAL_WIDTH", &logicalWidthOverride)) {
                logicalWidth = std::max(1, logicalWidthOverride);
                logicalWidthExplicit = true;
            }

            int logicalHeightOverride;
            if (configGetInt(&resolutionConfig, "SCALER", "LOGICAL_HEIGHT", &logicalHeightOverride)) {
                logicalHeight = std::max(1, logicalHeightOverride);
                logicalHeightExplicit = true;
            }

            configGetBool(&resolutionConfig, "SCALER", "INTEGER_SCALING", &integerScaling);

            configGetBool(&resolutionConfig, "DIAGNOSTICS", "ENABLE", &diagnosticsEnabled);

            char* diagnosticsVerbosityValue;
            if (configGetString(&resolutionConfig, "DIAGNOSTICS", "VERBOSITY", &diagnosticsVerbosityValue)) {
                diagnosticsLevel = diagnosticsParseLevel(diagnosticsVerbosityValue);
            }

            if (!configGetString(&resolutionConfig, "DIAGNOSTICS", "LOG_FILE", &diagnosticsLogFileValue)) {
                diagnosticsLogFileValue = nullptr;
            }
        }
        diagnosticsInit(diagnosticsEnabled, diagnosticsLevel, diagnosticsLogFileValue);
        configFree(&resolutionConfig);
    } else {
        diagnosticsInit(false, DiagnosticsLevel::Info, nullptr);
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(
            DiagnosticsLevel::Info,
            "BOOT",
            "diagnostics enabled=%d level=%s log=%s",
            diagnosticsEnabled ? 1 : 0,
            diagnosticsLevelToString(diagnosticsLevel),
            diagnosticsGetLogPath());
    }

    if (!preserveOriginalLogicalSize && logicalScale > 1) {
        if (!logicalWidthExplicit) {
            logicalWidth = displayScalerGetDefaultLogicalWidth() * logicalScale;
        }

        if (!logicalHeightExplicit) {
            logicalHeight = displayScalerGetDefaultLogicalHeight() * logicalScale;
        }
    }

    logicalWidth = std::max(1, logicalWidth);
    logicalHeight = std::max(1, logicalHeight);

    const int requiredWindowWidth = (logicalWidth + scale - 1) / scale;
    const int requiredWindowHeight = (logicalHeight + scale - 1) / scale;
    width = std::max(width, requiredWindowWidth);
    height = std::max(height, requiredWindowHeight);

    displayScalerInit(logicalWidth, logicalHeight);
    displayScalerSetIntegerScaling(integerScaling);

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(
            DiagnosticsLevel::Info,
            "BOOT",
            "init logical=%dx%d fullscreen=%d scale=%d integerScaling=%d legacyLogical=%d logicalScale=%d",
            logicalWidth,
            logicalHeight,
            fullscreen ? 1 : 0,
            scale,
            integerScaling ? 1 : 0,
            preserveOriginalLogicalSize ? 1 : 0,
            logicalScale);
    }

    if (_GNW95_init_window(width, height, fullscreen, scale) == -1) {
        return -1;
    }

    if (directDrawInit(logicalWidth, logicalHeight, bpp) == -1) {
        return -1;
    }

    const Rect& logical = displayScalerGetLogicalBounds();
    rectCopy(&_scr_size, &logical);

    _mouse_blit_trans = nullptr;
    _scr_blit = _GNW95_ShowRect;
    _zero_mem = _GNW95_zero_vid_mem;
    _mouse_blit = _GNW95_ShowRect;

    return 0;
}

// 0x4CAECC
int _init_vesa_mode(int width, int height)
{
    return _GNW95_init_mode_ex(width, height, 8);
}

// 0x4CAEDC
int _GNW95_init_window(int width, int height, bool fullscreen, int scale)
{
    if (gSdlWindow == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

        if (fullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN;
        }

        const int physicalWidth = width * scale;
        const int physicalHeight = height * scale;

        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, physicalWidth, physicalHeight, windowFlags);
        if (gSdlWindow == nullptr) {
            return -1;
        }

        if (!createRenderer()) {
            destroyRenderer();

            SDL_DestroyWindow(gSdlWindow);
            gSdlWindow = nullptr;

            return -1;
        }

        syncPhysicalSizeWithRenderer();
    } else {
        int physicalWidth;
        int physicalHeight;
        SDL_GetWindowSize(gSdlWindow, &physicalWidth, &physicalHeight);
        displayScalerUpdatePhysicalSize(physicalWidth, physicalHeight);
    }

    return 0;
}

// 0x4CAF9C
int directDrawInit(int width, int height, int bpp)
{
    if (gSdlSurface != nullptr) {
        unsigned char* palette = directDrawGetPalette();
        directDrawFree();

        if (directDrawInit(width, height, bpp) == -1) {
            return -1;
        }

        directDrawSetPalette(palette);

        return 0;
    }

    gSdlSurface = SDL_CreateRGBSurface(0, width, height, bpp, 0, 0, 0, 0);

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    return 0;
}

// 0x4CB1B0
void directDrawFree()
{
    if (gSdlSurface != nullptr) {
        SDL_FreeSurface(gSdlSurface);
        gSdlSurface = nullptr;
    }
}

// 0x4CB310
void directDrawSetPaletteInRange(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        if (!gTrueColorRendererActive && gSdlTextureSurface != nullptr) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        }

        windowNotifyPaletteChanged();
    }
}

// 0x4CB568
void directDrawSetPalette(unsigned char* palette)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        if (!gTrueColorRendererActive && gSdlTextureSurface != nullptr) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        }

        windowNotifyPaletteChanged();
    }
}

// 0x4CB68C
unsigned char* directDrawGetPalette()
{
    // 0x6ACA24
    static unsigned char palette[768];

    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color* colors = gSdlSurface->format->palette->colors;

        for (int index = 0; index < 256; index++) {
            SDL_Color* color = &(colors[index]);
            palette[index * 3] = color->r >> 2;
            palette[index * 3 + 1] = color->g >> 2;
            palette[index * 3 + 2] = color->b >> 2;
        }
    }

    return palette;
}

// 0x4CB850
void _GNW95_ShowRect(unsigned char* src, int srcPitch, int a3, int srcX, int srcY, int srcWidth, int srcHeight, int destX, int destY)
{
    if (gSdlSurface == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    const Rect& logicalBounds = displayScalerGetLogicalBounds();

    Rect destRect;
    destRect.left = destX;
    destRect.top = destY;
    destRect.right = destX + srcWidth - 1;
    destRect.bottom = destY + srcHeight - 1;

    if (rectIntersection(&destRect, &logicalBounds, &destRect) == -1) {
        return;
    }

    const int clippedWidth = rectGetWidth(&destRect);
    const int clippedHeight = rectGetHeight(&destRect);
    if (clippedWidth <= 0 || clippedHeight <= 0) {
        return;
    }

    const int srcOffsetX = destRect.left - destX;
    const int srcOffsetY = destRect.top - destY;
    unsigned char* srcStart = src + srcPitch * (srcY + srcOffsetY) + (srcX + srcOffsetX);

    if (gTrueColorRendererActive) {
        blitIndexedBufferToTextureSurface(srcStart, srcPitch, clippedWidth, clippedHeight, destRect.left, destRect.top);
        return;
    }

    blitBufferToBuffer(srcStart, clippedWidth, clippedHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destRect.top + destRect.left, gSdlSurface->pitch);

    SDL_Rect sdlRect;
    sdlRect.x = destRect.left;
    sdlRect.y = destRect.top;
    sdlRect.w = clippedWidth;
    sdlRect.h = clippedHeight;

    SDL_BlitSurface(gSdlSurface, &sdlRect, gSdlTextureSurface, &sdlRect);
}

// Clears drawing surface.
//
// 0x4CBBC8
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
        return;
    }

    if (gTrueColorRendererActive) {
        if (gSdlTextureSurface != nullptr) {
            SDL_FillRect(gSdlTextureSurface, nullptr, SDL_MapRGBA(gSdlTextureSurface->format, 0, 0, 0, 255));
        }
        return;
    }

    unsigned char* surface = (unsigned char*)gSdlSurface->pixels;
    for (int y = 0; y < gSdlSurface->h; y++) {
        memset(surface, 0, gSdlSurface->w);
        surface += gSdlSurface->pitch;
    }

    SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
}

int screenGetWidth()
{
    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    return logicalSpace.width;
}

int screenGetHeight()
{
    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    return logicalSpace.height;
}

int screenGetVisibleHeight()
{
    int windowBottomMargin = 0;

    if (!gInterfaceBarMode) {
        windowBottomMargin = INTERFACE_BAR_HEIGHT;
    }
    return screenGetHeight() - windowBottomMargin;
}

int screenGetPhysicalWidth()
{
    PhysicalSpace physicalSpace = displayScalerGetPhysicalSpace();
    return physicalSpace.width;
}

int screenGetPhysicalHeight()
{
    PhysicalSpace physicalSpace = displayScalerGetPhysicalSpace();
    return physicalSpace.height;
}

static bool createRenderer()
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == nullptr) {
        return false;
    }

    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    Uint32 requestedFormat = gTrueColorRendererRequested ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB888;
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, requestedFormat, SDL_TEXTUREACCESS_STREAMING, logicalSpace.width, logicalSpace.height);
    if (gSdlTexture == nullptr && gTrueColorRendererRequested) {
        requestedFormat = SDL_PIXELFORMAT_RGB888;
        gSdlTexture = SDL_CreateTexture(gSdlRenderer, requestedFormat, SDL_TEXTUREACCESS_STREAMING, logicalSpace.width, logicalSpace.height);
        gTrueColorRendererActive = false;
    }

    if (gSdlTexture == nullptr) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    if (gTrueColorRendererRequested && format != SDL_PIXELFORMAT_ARGB8888) {
        gTrueColorRendererActive = false;
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "true color renderer unsupported, falling back to %s", SDL_GetPixelFormatName(format));
        }
    } else {
        gTrueColorRendererActive = gTrueColorRendererRequested;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, logicalSpace.width, logicalSpace.height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == nullptr) {
        return false;
    }

    return true;
}

static void destroyRenderer()
{
    if (gSdlTextureSurface != nullptr) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = nullptr;
    }

    if (gSdlTexture != nullptr) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = nullptr;
    }

    if (gSdlRenderer != nullptr) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = nullptr;
    }

    gTrueColorRendererActive = false;
}

static void syncPhysicalSizeWithRenderer()
{
    if (gSdlRenderer == nullptr) {
        return;
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (SDL_GetRendererOutputSize(gSdlRenderer, &outputWidth, &outputHeight) != 0) {
        return;
    }

    PhysicalSpace physicalSpace = displayScalerGetPhysicalSpace();
    if (physicalSpace.width != outputWidth || physicalSpace.height != outputHeight) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(
                DiagnosticsLevel::Info,
                "RENDERER",
                "renderer output resized %dx%d -> %dx%d",
                physicalSpace.width,
                physicalSpace.height,
                outputWidth,
                outputHeight);
        }
        displayScalerUpdatePhysicalSize(outputWidth, outputHeight);
    }
}

static bool rectEquals(const Rect& a, const Rect& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static void logViewportIfChanged(const Rect& viewport)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        return;
    }

    if (gHasRenderViewport && rectEquals(gLastRenderViewport, viewport)) {
        return;
    }

    gLastRenderViewport = viewport;
    gHasRenderViewport = true;

    const int width = rectGetWidth(&viewport);
    const int height = rectGetHeight(&viewport);

    diagnosticsLog(
        DiagnosticsLevel::Info,
        "RENDERER",
        "viewport (%d,%d)-(%d,%d) size=%dx%d",
        viewport.left,
        viewport.top,
        viewport.right,
        viewport.bottom,
        width,
        height);
}

void blitIndexedBufferToTextureSurface(const unsigned char* src, int srcPitch, int width, int height, int destX, int destY)
{
    if (gSdlTextureSurface == nullptr || width <= 0 || height <= 0) {
        return;
    }

    const int destBytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    uint8_t* destPixels = static_cast<uint8_t*>(gSdlTextureSurface->pixels) + destY * gSdlTextureSurface->pitch + destX * destBytesPerPixel;

    for (int row = 0; row < height; row++) {
        const unsigned char* srcRow = src + srcPitch * row;
        uint8_t* destRow = destPixels + gSdlTextureSurface->pitch * row;
        for (int col = 0; col < width; col++) {
            uint32_t argb = paletteIndexToArgb(srcRow[col]);
            if (destBytesPerPixel == 4) {
                reinterpret_cast<uint32_t*>(destRow)[col] = argb;
            } else {
                // Assume RGB888
                uint8_t* pixel = destRow + col * destBytesPerPixel;
                pixel[0] = static_cast<uint8_t>(argb >> 16);
                pixel[1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
                pixel[2] = static_cast<uint8_t>(argb & 0xFF);
            }
        }
    }
}

void blitTrueColorBufferToTextureSurface(const uint32_t* src, int srcPitch, int width, int height, int destX, int destY)
{
    if (gSdlTextureSurface == nullptr || src == nullptr || width <= 0 || height <= 0) {
        return;
    }

    const int destBytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    uint8_t* destPixels = static_cast<uint8_t*>(gSdlTextureSurface->pixels) + destY * gSdlTextureSurface->pitch + destX * destBytesPerPixel;

    for (int row = 0; row < height; row++) {
        const uint32_t* srcRow = src + srcPitch * row;
        uint8_t* destRow = destPixels + gSdlTextureSurface->pitch * row;
        if (destBytesPerPixel == 4) {
            memcpy(destRow, srcRow, width * sizeof(uint32_t));
        } else {
            for (int col = 0; col < width; col++) {
                uint32_t pixel = srcRow[col];
                uint8_t* dest = destRow + col * destBytesPerPixel;
                dest[0] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
                dest[1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
                dest[2] = static_cast<uint8_t>(pixel & 0xFF);
            }
        }
    }
}

void handleWindowSizeChanged()
{
    if (gSdlWindow == nullptr) {
        return;
    }

    int physicalWidth;
    int physicalHeight;
    SDL_GetWindowSize(gSdlWindow, &physicalWidth, &physicalHeight);

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(
            DiagnosticsLevel::Info,
            "WINDOW",
            "window size changed to %dx%d",
            physicalWidth,
            physicalHeight);
    }

    displayScalerUpdatePhysicalSize(physicalWidth, physicalHeight);

    destroyRenderer();

    const Rect& logical = displayScalerGetLogicalBounds();
    rectCopy(&_scr_size, &logical);

    createRenderer();

    if (!gTrueColorRendererActive && gSdlTextureSurface != nullptr && gSdlSurface != nullptr) {
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    }

    syncPhysicalSizeWithRenderer();
}

void renderPresent()
{
    syncPhysicalSizeWithRenderer();

    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    SDL_Rect srcRect;
    srcRect.x = 0;
    srcRect.y = 0;
    srcRect.w = logicalSpace.width;
    srcRect.h = logicalSpace.height;

    SDL_UpdateTexture(gSdlTexture, nullptr, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
    const Rect& viewport = displayScalerGetPhysicalViewport();
    logViewportIfChanged(viewport);
    SDL_Rect destRect;
    destRect.x = viewport.left;
    destRect.y = viewport.top;
    destRect.w = rectGetWidth(&viewport);
    destRect.h = rectGetHeight(&viewport);

    SDL_Rect letterboxRects[4];
    int rectCount = 0;

    if (destRect.y > 0) {
        SDL_Rect& topRect = letterboxRects[rectCount++];
        topRect.x = 0;
        topRect.y = 0;
        topRect.w = screenGetPhysicalWidth();
        topRect.h = destRect.y;
    }

    const int bottomStart = destRect.y + destRect.h;
    const int physicalHeight = screenGetPhysicalHeight();
    if (bottomStart < physicalHeight) {
        SDL_Rect& bottomRect = letterboxRects[rectCount++];
        bottomRect.x = 0;
        bottomRect.y = bottomStart;
        bottomRect.w = screenGetPhysicalWidth();
        bottomRect.h = physicalHeight - bottomStart;
    }

    if (destRect.x > 0) {
        SDL_Rect& leftRect = letterboxRects[rectCount++];
        leftRect.x = 0;
        leftRect.y = destRect.y;
        leftRect.w = destRect.x;
        leftRect.h = destRect.h;
    }

    const int rightStart = destRect.x + destRect.w;
    const int physicalWidth = screenGetPhysicalWidth();
    if (rightStart < physicalWidth) {
        SDL_Rect& rightRect = letterboxRects[rectCount++];
        rightRect.x = rightStart;
        rightRect.y = destRect.y;
        rightRect.w = physicalWidth - rightStart;
        rightRect.h = destRect.h;
    }

    if (rectCount > 0 && diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(
            DiagnosticsLevel::Trace,
            "RENDERER",
            "letterbox count=%d dest=(%d,%d %dx%d)",
            rectCount,
            destRect.x,
            destRect.y,
            destRect.w,
            destRect.h);
    }

    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    if (rectCount > 0) {
        SDL_RenderFillRects(gSdlRenderer, letterboxRects, rectCount);
    }

    SDL_RenderCopy(gSdlRenderer, gSdlTexture, &srcRect, &destRect);
    SDL_RenderPresent(gSdlRenderer);
}

} // namespace fallout
