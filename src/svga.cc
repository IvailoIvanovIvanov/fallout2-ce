#include "svga.h"

#include <algorithm>
#include <cstdint>
#include <limits.h>
#include <string.h>

#include <SDL.h>

#include "config.h"
#include "diagnostics.h"
#include "display_scaler.h"
#include "draw.h"
#include "geometry.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "render_trace.h"
#include "virtual_input.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

static bool createRenderer();
static void destroyRenderer();
static void syncPhysicalSizeWithRenderer();
static bool rectEquals(const Rect& a, const Rect& b);
static void logViewportIfChanged(const Rect& viewport);
static bool copySurfaceRectToTexture(SDL_Texture* texture, SDL_Surface* surface, const Rect& rect);
static void logTextureSurfaceRectStats(const Rect& rect, const char* label);
static void logTextureUploadRectStats(const Rect& rect, const char* label);
static uint8_t expandPaletteComponent(uint8_t value);
static void logPaletteUploadSamples(int start, int count, const unsigned char* palette);
static void updateTexturePaletteRange(int start, int count, const unsigned char* palette);

static Rect gLastRenderViewport = { 0, 0, -1, -1 };
static bool gHasRenderViewport = false;
static uint32_t gTexturePalette[256] = {};
static int gTextureSurfaceLogBudget = 16;
static int gTextureUploadLogBudget = 8;
static int gPaletteUploadLogBudget = 64;
static int gTextureUploadFailureLogBudget = 4;
static int gTextureUploadFallbackLogBudget = 4;
static int gIndexedBlitLogBudget = 16;
static int gIndexedPaletteMismatchLogBudget = 4;

static Rect makeSurfaceBoundsRect(int width, int height)
{
    Rect bounds = { 0, 0, -1, -1 };
    if (width > 0 && height > 0) {
        bounds.right = width - 1;
        bounds.bottom = height - 1;
    }
    return bounds;
}

static bool clipRectToSurface(Rect* rect, const Rect& logicalBounds, const Rect& surfaceBounds)
{
    Rect clipped;
    if (rectIntersection(rect, &logicalBounds, &clipped) == -1) {
        return false;
    }

    if (rectIntersection(&clipped, &surfaceBounds, rect) == -1) {
        return false;
    }

    return true;
}

static bool copySurfaceRectToTexture(SDL_Texture* texture, SDL_Surface* surface, const Rect& rect)
{
    if (texture == nullptr || surface == nullptr || surface->format == nullptr) {
        return false;
    }

    const int width = rectGetWidth(&rect);
    const int height = rectGetHeight(&rect);
    if (width <= 0 || height <= 0) {
        return false;
    }

    SDL_Rect sdlRect;
    sdlRect.x = rect.left;
    sdlRect.y = rect.top;
    sdlRect.w = width;
    sdlRect.h = height;

    void* destination = nullptr;
    int destinationPitch = 0;
    if (SDL_LockTexture(texture, &sdlRect, &destination, &destinationPitch) != 0 || destination == nullptr) {
        return false;
    }

    const uint8_t* sourcePixels = static_cast<const uint8_t*>(surface->pixels);
    const int sourcePitch = surface->pitch;
    const int bytesPerPixel = surface->format->BytesPerPixel;
    uint8_t* destPixels = static_cast<uint8_t*>(destination);
    for (int row = 0; row < height; row++) {
        const uint8_t* sourceRow = sourcePixels + (rect.top + row) * sourcePitch + rect.left * bytesPerPixel;
        memcpy(destPixels + row * destinationPitch, sourceRow, width * bytesPerPixel);
    }

    SDL_UnlockTexture(texture);
    return true;
}

static void logTextureSurfaceRectStats(const Rect& rect, const char* label)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace) || label == nullptr || gSdlTextureSurface == nullptr || gTextureSurfaceLogBudget <= 0) {
        return;
    }

    SDL_PixelFormat* format = gSdlTextureSurface->format;
    if (format == nullptr || format->BytesPerPixel != 4) {
        return;
    }

    Rect clipped;
    rectCopy(&clipped, &rect);

    const Rect logicalBounds = displayScalerGetLogicalBounds();
    Rect surfaceBounds = makeSurfaceBoundsRect(gSdlTextureSurface->w, gSdlTextureSurface->h);
    if (!clipRectToSurface(&clipped, logicalBounds, surfaceBounds)) {
        return;
    }

    const int width = rectGetWidth(&clipped);
    const int height = rectGetHeight(&clipped);
    if (width <= 0 || height <= 0) {
        return;
    }

    const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (totalPixels == 0) {
        return;
    }

    const uint8_t* pixels = static_cast<const uint8_t*>(gSdlTextureSurface->pixels);
    if (pixels == nullptr) {
        return;
    }

    const int pitch = gSdlTextureSurface->pitch;
    const int bytesPerPixel = format->BytesPerPixel;

    uint32_t minValue = 0xFFFFFFFF;
    uint32_t maxValue = 0x00000000;
    unsigned long long checksum = 0;

    const size_t sampleStart = 0;
    const size_t sampleMiddle = totalPixels / 2;
    const size_t sampleEnd = totalPixels - 1;
    uint32_t sampleStartValue = 0;
    uint32_t sampleMiddleValue = 0;
    uint32_t sampleEndValue = 0;

    size_t currentIndex = 0;
    for (int y = 0; y < height; y++) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(pixels + (clipped.top + y) * pitch + clipped.left * bytesPerPixel);
        for (int x = 0; x < width; x++, currentIndex++) {
            const uint32_t value = row[x];
            checksum += value;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);

            if (currentIndex == sampleStart) {
                sampleStartValue = value;
            }
            if (currentIndex == sampleMiddle) {
                sampleMiddleValue = value;
            }
            if (currentIndex == sampleEnd) {
                sampleEndValue = value;
            }
        }
    }

    diagnosticsLog(DiagnosticsLevel::Trace,
        "RENDERER",
        "texture_surface stats label=%s rect=(%d,%d %dx%d) min=0x%08X max=0x%08X checksum=0x%llX samples=0x%08X,0x%08X,0x%08X",
        label,
        clipped.left,
        clipped.top,
        width,
        height,
        minValue,
        maxValue,
        checksum,
        sampleStartValue,
        sampleMiddleValue,
        sampleEndValue);

    gTextureSurfaceLogBudget--;
    if (gTextureSurfaceLogBudget == 0) {
        diagnosticsLog(DiagnosticsLevel::Trace, "RENDERER", "texture_surface logging budget exhausted");
    }
}

static void logTextureUploadRectStats(const Rect& rect, const char* label)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace) || label == nullptr || gSdlTexture == nullptr || gTextureUploadLogBudget <= 0) {
        return;
    }

    Rect clipped;
    rectCopy(&clipped, &rect);

    const Rect logicalBounds = displayScalerGetLogicalBounds();
    int textureWidth = 0;
    int textureHeight = 0;
    if (SDL_QueryTexture(gSdlTexture, nullptr, nullptr, &textureWidth, &textureHeight) != 0) {
        return;
    }

    Rect textureBounds = makeSurfaceBoundsRect(textureWidth, textureHeight);
    if (!clipRectToSurface(&clipped, logicalBounds, textureBounds)) {
        return;
    }

    const int width = rectGetWidth(&clipped);
    const int height = rectGetHeight(&clipped);
    if (width <= 0 || height <= 0) {
        return;
    }

    SDL_Rect sdlRect;
    sdlRect.x = clipped.left;
    sdlRect.y = clipped.top;
    sdlRect.w = width;
    sdlRect.h = height;

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(gSdlTexture, &sdlRect, &pixels, &pitch) != 0 || pixels == nullptr) {
        return;
    }

    const uint8_t* base = static_cast<const uint8_t*>(pixels);
    const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);

    uint32_t minValue = 0xFFFFFFFF;
    uint32_t maxValue = 0x00000000;
    unsigned long long checksum = 0;

    const size_t sampleStart = 0;
    const size_t sampleMiddle = totalPixels / 2;
    const size_t sampleEnd = totalPixels - 1;
    uint32_t sampleStartValue = 0;
    uint32_t sampleMiddleValue = 0;
    uint32_t sampleEndValue = 0;

    size_t currentIndex = 0;
    for (int y = 0; y < height; y++) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(base + y * pitch);
        for (int x = 0; x < width; x++, currentIndex++) {
            const uint32_t value = row[x];
            checksum += value;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);

            if (currentIndex == sampleStart) {
                sampleStartValue = value;
            }
            if (currentIndex == sampleMiddle) {
                sampleMiddleValue = value;
            }
            if (currentIndex == sampleEnd) {
                sampleEndValue = value;
            }
        }
    }

    SDL_UnlockTexture(gSdlTexture);

    diagnosticsLog(DiagnosticsLevel::Trace,
        "RENDERER",
        "texture_upload stats label=%s rect=(%d,%d %dx%d) min=0x%08X max=0x%08X checksum=0x%llX samples=0x%08X,0x%08X,0x%08X",
        label,
        clipped.left,
        clipped.top,
        width,
        height,
        minValue,
        maxValue,
        checksum,
        sampleStartValue,
        sampleMiddleValue,
        sampleEndValue);

    gTextureUploadLogBudget--;
    if (gTextureUploadLogBudget == 0) {
        diagnosticsLog(DiagnosticsLevel::Trace, "RENDERER", "texture_upload logging budget exhausted");
    }
}

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

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

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

// 0x4CAE1C
int _GNW95_init_mode_ex(int width, int height, int bpp)
{
    bool fullscreen = true;
    int scale = 1;
    int logicalWidth = width;
    int logicalHeight = height;
    bool integerScaling = false;
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

            int logicalWidthOverride;
            if (configGetInt(&resolutionConfig, "SCALER", "LOGICAL_WIDTH", &logicalWidthOverride)) {
                logicalWidth = std::max(1, logicalWidthOverride);
            }

            int logicalHeightOverride;
            if (configGetInt(&resolutionConfig, "SCALER", "LOGICAL_HEIGHT", &logicalHeightOverride)) {
                logicalHeight = std::max(1, logicalHeightOverride);
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

    displayScalerInit(logicalWidth, logicalHeight);
    displayScalerSetIntegerScaling(integerScaling);

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(
            DiagnosticsLevel::Info,
            "BOOT",
            "init logical=%dx%d fullscreen=%d scale=%d integerScaling=%d",
            logicalWidth,
            logicalHeight,
            fullscreen ? 1 : 0,
            scale,
            integerScaling ? 1 : 0);
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

    unsigned char palette[256 * 3];
    for (int index = 0; index < 256; index++) {
        unsigned char value = static_cast<unsigned char>(index >> 2);
        palette[index * 3 + 0] = value;
        palette[index * 3 + 1] = value;
        palette[index * 3 + 2] = value;
    }
    updateTexturePaletteRange(0, 256, palette);

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
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        updateTexturePaletteRange(start, count, palette);

        if (windowIsVirtualScreenEnabled()) {
            windowVirtualScreenInvalidateAll();
        }
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
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        updateTexturePaletteRange(0, 256, palette);

        if (windowIsVirtualScreenEnabled()) {
            windowVirtualScreenInvalidateAll();
        }
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

    blitBufferToBuffer(srcStart, clippedWidth, clippedHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destRect.top + destRect.left, gSdlSurface->pitch);

    SDL_Rect sdlRect;
    sdlRect.x = destRect.left;
    sdlRect.y = destRect.top;
    sdlRect.w = clippedWidth;
    sdlRect.h = clippedHeight;

    SDL_BlitSurface(gSdlSurface, &sdlRect, gSdlTextureSurface, &sdlRect);
}

void blitIndexedRectToTexture(const unsigned char* src, int srcPitch, const Rect& rect)
{
    if (src == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    const int width = rectGetWidth(&rect);
    const int height = rectGetHeight(&rect);
    if (width <= 0 || height <= 0) {
        return;
    }

    const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (totalPixels == 0) {
        return;
    }

    if (gSdlTextureSurface->format == nullptr || gSdlTextureSurface->format->BytesPerPixel != 4) {
        return;
    }

    const int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    unsigned char* destPixels = static_cast<unsigned char*>(gSdlTextureSurface->pixels);

    const bool logBlitStats = diagnosticsWouldLog(DiagnosticsLevel::Trace) && gIndexedBlitLogBudget > 0;
    unsigned int srcMin = 255;
    unsigned int srcMax = 0;
    unsigned int srcSampleStart = 0;
    unsigned int srcSampleMiddle = 0;
    unsigned int srcSampleEnd = 0;
    uint32_t mappedSampleStart = 0;
    uint32_t mappedSampleMiddle = 0;
    uint32_t mappedSampleEnd = 0;
    const size_t sampleStart = 0;
    const size_t sampleMiddle = totalPixels / 2;
    const size_t sampleEnd = totalPixels - 1;
    size_t currentIndex = 0;

    for (int row = 0; row < height; row++) {
        const unsigned char* srcRow = src + (rect.top + row) * srcPitch + rect.left;
        uint32_t* destRow = reinterpret_cast<uint32_t*>(destPixels + (rect.top + row) * gSdlTextureSurface->pitch + rect.left * bytesPerPixel);

        for (int column = 0; column < width; column++) {
            const unsigned int paletteIndex = srcRow[column];
            const uint32_t mappedColor = gTexturePalette[paletteIndex];
            destRow[column] = mappedColor;

            if (logBlitStats) {
                srcMin = std::min(srcMin, paletteIndex);
                srcMax = std::max(srcMax, paletteIndex);

                if (currentIndex == sampleStart) {
                    srcSampleStart = paletteIndex;
                    mappedSampleStart = mappedColor;
                }
                if (currentIndex == sampleMiddle) {
                    srcSampleMiddle = paletteIndex;
                    mappedSampleMiddle = mappedColor;
                }
                if (currentIndex == sampleEnd) {
                    srcSampleEnd = paletteIndex;
                    mappedSampleEnd = mappedColor;
                }
            }

            currentIndex++;
        }
    }

    if (logBlitStats) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "RENDERER",
            "indexed_blit stats rect=(%d,%d %dx%d) src_min=%u src_max=%u src_samples=%u,%u,%u mapped_samples=0x%08X,0x%08X,0x%08X",
            rect.left,
            rect.top,
            width,
            height,
            srcMin,
            srcMax,
            srcSampleStart,
            srcSampleMiddle,
            srcSampleEnd,
            mappedSampleStart,
            mappedSampleMiddle,
            mappedSampleEnd);

        const bool srcHasNonZeroSample = srcMax > 0 || srcSampleStart > 0 || srcSampleMiddle > 0 || srcSampleEnd > 0;
        const bool allMappedZero = mappedSampleStart == 0 && mappedSampleMiddle == 0 && mappedSampleEnd == 0;
        const bool sampleMismatch = (srcSampleStart > 0 && mappedSampleStart == 0) || (srcSampleMiddle > 0 && mappedSampleMiddle == 0)
            || (srcSampleEnd > 0 && mappedSampleEnd == 0);
        if (srcHasNonZeroSample && (allMappedZero || sampleMismatch) && gIndexedPaletteMismatchLogBudget > 0) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "RENDERER",
                "indexed_blit palette mismatch rect=(%d,%d %dx%d) src_samples=%u,%u,%u mapped=0x%08X,0x%08X,0x%08X",
                rect.left,
                rect.top,
                width,
                height,
                srcSampleStart,
                srcSampleMiddle,
                srcSampleEnd,
                mappedSampleStart,
                mappedSampleMiddle,
                mappedSampleEnd);
            gIndexedPaletteMismatchLogBudget--;
            if (gIndexedPaletteMismatchLogBudget == 0) {
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "indexed_blit palette mismatch logging budget exhausted");
            }
        }

        gIndexedBlitLogBudget--;
        if (gIndexedBlitLogBudget == 0) {
            diagnosticsLog(DiagnosticsLevel::Trace, "RENDERER", "indexed_blit logging budget exhausted");
        }
    }

    logTextureSurfaceRectStats(rect, "after_indexed_blit");
}

int blitTrueColorRectToTexture(const uint32_t* src, const unsigned char* mask, int srcPitch, const Rect& rect)
{
    if (src == nullptr || gSdlTextureSurface == nullptr) {
        return 0;
    }

    const int width = rectGetWidth(&rect);
    const int height = rectGetHeight(&rect);
    if (width <= 0 || height <= 0) {
        return 0;
    }

    if (gSdlTextureSurface->format == nullptr || gSdlTextureSurface->format->BytesPerPixel != 4) {
        return 0;
    }

    const int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    unsigned char* destPixels = static_cast<unsigned char*>(gSdlTextureSurface->pixels);
    int pixelsWritten = 0;

    for (int row = 0; row < height; row++) {
        const uint32_t* srcRow = src + row * srcPitch;
        const unsigned char* maskRow = mask != nullptr ? mask + row * srcPitch : nullptr;
        uint32_t* destRow = reinterpret_cast<uint32_t*>(destPixels + (rect.top + row) * gSdlTextureSurface->pitch + rect.left * bytesPerPixel);

        if (maskRow == nullptr) {
            memcpy(destRow, srcRow, width * sizeof(uint32_t));
            pixelsWritten += width;
            continue;
        }

        for (int column = 0; column < width; column++) {
            if (maskRow[column] != 0) {
                destRow[column] = srcRow[column];
                pixelsWritten++;
            }
        }
    }

    return pixelsWritten;
}

// Clears drawing surface.
//
// 0x4CBBC8
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
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

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, logicalSpace.width, logicalSpace.height);
    if (gSdlTexture == nullptr) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
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

static uint8_t expandPaletteComponent(uint8_t value)
{
    if (value > 63) {
        return value;
    }

    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

static void logPaletteUploadSamples(int start, int count, const unsigned char* palette)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace) || palette == nullptr || count <= 0) {
        return;
    }

    if (gPaletteUploadLogBudget <= 0) {
        return;
    }

    static const int kSampleIndices[] = { 0, 1, 11, 110, 111, 205, 226, 255 };
    bool loggedSamples = false;

    for (int sampleIndex : kSampleIndices) {
        if (sampleIndex < start || sampleIndex >= start + count) {
            continue;
        }

        int paletteOffset = (sampleIndex - start) * 3;
        uint8_t rawR = palette[paletteOffset + 0];
        uint8_t rawG = palette[paletteOffset + 1];
        uint8_t rawB = palette[paletteOffset + 2];

        uint8_t expandedR = expandPaletteComponent(rawR);
        uint8_t expandedG = expandPaletteComponent(rawG);
        uint8_t expandedB = expandPaletteComponent(rawB);

        uint32_t argb = gTexturePalette[sampleIndex];

        diagnosticsLog(DiagnosticsLevel::Trace,
            "RENDERER",
            "palette_upload index=%3d raw=(%3u,%3u,%3u) expanded=(%3u,%3u,%3u) argb=0x%08X",
            sampleIndex,
            rawR,
            rawG,
            rawB,
            expandedR,
            expandedG,
            expandedB,
            argb);

        loggedSamples = true;
    }

    if (!loggedSamples) {
        return;
    }

    int rawMin = 255;
    int rawMax = 0;
    const int totalComponents = count * 3;
    for (int offset = 0; offset < totalComponents; offset++) {
        rawMin = std::min(rawMin, static_cast<int>(palette[offset]));
        rawMax = std::max(rawMax, static_cast<int>(palette[offset]));
    }

    diagnosticsLog(DiagnosticsLevel::Trace,
        "RENDERER",
        "palette_upload range start=%d count=%d raw_bounds=%d-%d",
        start,
        count,
        rawMin,
        rawMax);

    gPaletteUploadLogBudget--;

    if (gPaletteUploadLogBudget == 0) {
        diagnosticsLog(DiagnosticsLevel::Trace, "RENDERER", "palette_upload logging budget exhausted");
    }
}

static void updateTexturePaletteRange(int start, int count, const unsigned char* palette)
{
    if (palette == nullptr || count <= 0 || gSdlTextureSurface == nullptr || gSdlTextureSurface->format == nullptr) {
        return;
    }

    SDL_PixelFormat* format = gSdlTextureSurface->format;
    for (int index = 0; index < count; index++) {
        int paletteIndex = start + index;
        if (paletteIndex < 0 || paletteIndex >= 256) {
            continue;
        }

        int paletteOffset = index * 3;
        uint8_t r = expandPaletteComponent(palette[paletteOffset + 0]);
        uint8_t g = expandPaletteComponent(palette[paletteOffset + 1]);
        uint8_t b = expandPaletteComponent(palette[paletteOffset + 2]);

        gTexturePalette[paletteIndex] = SDL_MapRGB(format, r, g, b);
    }

    logPaletteUploadSamples(start, count, palette);
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

    if (gSdlTextureSurface != nullptr && gSdlSurface != nullptr) {
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    }

    syncPhysicalSizeWithRenderer();
}

void renderPresent()
{
    syncPhysicalSizeWithRenderer();
    renderTraceCommitFrame();
    windowPresentVirtualScreen();

    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    SDL_Rect srcRect;
    srcRect.x = 0;
    srcRect.y = 0;
    srcRect.w = logicalSpace.width;
    srcRect.h = logicalSpace.height;

    Rect uploadRect;
    uploadRect.left = 0;
    uploadRect.top = 0;
    uploadRect.right = logicalSpace.width - 1;
    uploadRect.bottom = logicalSpace.height - 1;

    int textureUpdateResult = SDL_UpdateTexture(gSdlTexture, nullptr, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
    bool textureUploadOk = textureUpdateResult == 0;
    bool textureUploadFallbackUsed = false;
    if (!textureUploadOk) {
        if (gTextureUploadFailureLogBudget > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "SDL_UpdateTexture failed: %s", SDL_GetError());
            gTextureUploadFailureLogBudget--;
            if (gTextureUploadFailureLogBudget == 0) {
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "texture upload failure logging budget exhausted");
            }
        }

        if (copySurfaceRectToTexture(gSdlTexture, gSdlTextureSurface, uploadRect)) {
            textureUploadFallbackUsed = true;
            if (gTextureUploadFallbackLogBudget > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "SDL_UpdateTexture fallback copy succeeded for %dx%d", logicalSpace.width, logicalSpace.height);
            }
        } else if (gTextureUploadFallbackLogBudget > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "SDL_UpdateTexture fallback copy failed for %dx%d", logicalSpace.width, logicalSpace.height);
        }

        if (gTextureUploadFallbackLogBudget > 0) {
            gTextureUploadFallbackLogBudget--;
            if (gTextureUploadFallbackLogBudget == 0) {
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "texture upload fallback logging budget exhausted");
            }
        }
    }

    const char* textureUploadLabel = "after_texture_upload";
    if (!textureUploadOk) {
        textureUploadLabel = textureUploadFallbackUsed ? "after_texture_fallback_upload" : "after_texture_upload_failed";
    }
    logTextureUploadRectStats(uploadRect, textureUploadLabel);
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
    virtualInputRenderOverlay(gSdlRenderer);
    SDL_RenderPresent(gSdlRenderer);
}

} // namespace fallout
