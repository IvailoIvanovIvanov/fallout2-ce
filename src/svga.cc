#include "svga.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#include "render_commands.h"
#include "render_display_orchestrator.h"
#include "settings.h"
#include "virtual_input.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

static bool createRenderer();
static void destroyRenderer();
static void syncPhysicalSizeWithRenderer();
static bool ensurePresenterSurfaceMatchesBounds();
static void logPresenterSurfaceState(const char* reason, bool fullRes, int width, int height);
static bool rectEquals(const Rect& a, const Rect& b);
static void logViewportIfChanged(const Rect& viewport);
static bool copySurfaceRectToTexture(SDL_Texture* texture, SDL_Surface* surface, const Rect& rect);
static void logTextureSurfaceRectStats(const Rect& rect, const char* label);
static void logTextureUploadRectStats(const Rect& rect, const char* label);
static bool virtualAdapterTraceChannelEnabled();
static void logVirtualAdapterSourceSamples(const char* stage, const unsigned char* src, int width, int height, int pitch);
static uint8_t expandPaletteComponent(uint8_t value);
static void logPaletteUploadSamples(int start, int count, const unsigned char* palette);
static void updateTexturePaletteRange(int start, int count, const unsigned char* palette);
static bool isFullResPresenterActive();
static Rect getPresenterSurfaceBounds();
static bool resolvePresenterRect(const Rect& inputRect, Rect* outLogicalRect, Rect* outPresenterRect);

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
static bool gPresenterSurfaceFullRes = false;
static int gPresenterSurfaceWidth = 0;
static int gPresenterSurfaceHeight = 0;
static bool gPresenterSurfaceLogInitialized = false;
static bool gPresenterSurfaceLastFullRes = false;
static int gPresenterSurfaceLastWidth = 0;
static int gPresenterSurfaceLastHeight = 0;

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

    Rect clipped;
    rectCopy(&clipped, &rect);

    const Rect presenterBounds = getPresenterSurfaceBounds();
    Rect surfaceBounds = makeSurfaceBoundsRect(surface->w, surface->h);
    if (!clipRectToSurface(&clipped, presenterBounds, surfaceBounds)) {
        return false;
    }

    const int width = rectGetWidth(&clipped);
    const int height = rectGetHeight(&clipped);
    if (width <= 0 || height <= 0) {
        return false;
    }

    SDL_Rect sdlRect;
    sdlRect.x = clipped.left;
    sdlRect.y = clipped.top;
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
        const uint8_t* sourceRow = sourcePixels + (clipped.top + row) * sourcePitch + clipped.left * bytesPerPixel;
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

    Rect logicalRect;
    Rect presenterRect;
    if (!resolvePresenterRect(rect, &logicalRect, &presenterRect)) {
        return;
    }

    Rect clipped;
    rectCopy(&clipped, &presenterRect);

    const Rect logicalBounds = getPresenterSurfaceBounds();
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

    const Rect logicalBounds = getPresenterSurfaceBounds();
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

static bool virtualAdapterTraceChannelEnabled()
{
    if (!settings.debug.virtual_adapter_trace) {
        return false;
    }

    if (!windowIsVirtualScreenEnabled()) {
        return false;
    }

    return diagnosticsWouldLog(DiagnosticsLevel::Trace);
}

static void logVirtualAdapterSourceSamples(const char* stage, const unsigned char* src, int width, int height, int pitch)
{
    if (!virtualAdapterTraceChannelEnabled() || stage == nullptr || src == nullptr) {
        return;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (totalPixels == 0) {
        return;
    }

    unsigned int minValue = 0xFF;
    unsigned int maxValue = 0;
    unsigned long long checksum = 0;
    const size_t sampleStart = 0;
    const size_t sampleMiddle = totalPixels / 2;
    const size_t sampleEnd = totalPixels - 1;
    unsigned int sampleStartValue = 0;
    unsigned int sampleMiddleValue = 0;
    unsigned int sampleEndValue = 0;

    size_t currentIndex = 0;
    for (int row = 0; row < height; row++) {
        const unsigned char* srcRow = src + row * pitch;
        for (int column = 0; column < width; column++, currentIndex++) {
            unsigned int value = srcRow[column];
            checksum += value;
            if (value < minValue) {
                minValue = value;
            }
            if (value > maxValue) {
                maxValue = value;
            }

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
        "VA_TRACE",
        "%s size=%dx%d min=%u max=%u checksum=0x%llX samples=%u,%u,%u",
        stage,
        width,
        height,
        minValue,
        maxValue,
        checksum,
        sampleStartValue,
        sampleMiddleValue,
        sampleEndValue);
}

static bool isFullResPresenterActive()
{
    if (!settings.system.virtual_adapter || !settings.system.virtual_adapter_fullres) {
        return false;
    }

    double scale = displayScalerGetScale();
    if (scale < 1.0 - 1.0e-4) {
        return false;
    }

    if (gSdlTexture == nullptr || gSdlTextureSurface == nullptr) {
        return false;
    }

    const Rect& viewport = displayScalerGetPhysicalViewport();
    const int viewportWidth = rectGetWidth(&viewport);
    const int viewportHeight = rectGetHeight(&viewport);
    if (viewportWidth <= 0 || viewportHeight <= 0) {
        return false;
    }

    if (gSdlTextureSurface->w < viewportWidth || gSdlTextureSurface->h < viewportHeight) {
        return false;
    }

    return true;
}

static Rect getPresenterSurfaceBounds()
{
    Rect bounds = { 0, 0, -1, -1 };

    if (isFullResPresenterActive()) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        int width = rectGetWidth(&viewport);
        int height = rectGetHeight(&viewport);
        if (width > 0 && height > 0) {
            bounds.right = width - 1;
            bounds.bottom = height - 1;
        }
    } else {
        const Rect& logical = displayScalerGetLogicalBounds();
        int width = rectGetWidth(&logical);
        int height = rectGetHeight(&logical);
        if (width > 0 && height > 0) {
            bounds.right = width - 1;
            bounds.bottom = height - 1;
        }
    }

    return bounds;
}

static bool resolvePresenterRect(const Rect& inputRect, Rect* outLogicalRect, Rect* outPresenterRect)
{
    const Rect& logicalBounds = displayScalerGetLogicalBounds();
    Rect logicalRect;
    rectCopy(&logicalRect, &inputRect);
    if (rectIntersection(&logicalRect, &logicalBounds, &logicalRect) == -1) {
        return false;
    }

    Rect presenterRect = logicalRect;
    if (isFullResPresenterActive()) {
        Rect mapped = displayScalerLogicalToPhysical(logicalRect);
        const Rect& viewport = displayScalerGetPhysicalViewport();

        mapped.left -= viewport.left;
        mapped.right -= viewport.left;
        mapped.top -= viewport.top;
        mapped.bottom -= viewport.top;

        presenterRect = mapped;
    }

    if (outLogicalRect != nullptr) {
        rectCopy(outLogicalRect, &logicalRect);
    }

    if (outPresenterRect != nullptr) {
        rectCopy(outPresenterRect, &presenterRect);
    }

    return true;
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

// Phase 7: GPU-resident overlay texture for HD content
// This texture receives HD tile/object content directly, bypassing CPU overlay buffers
SDL_Texture* gSdlOverlayTexture = nullptr;       // STREAMING texture for HD overlay
static bool gGpuOverlayEnabled = false;          // Runtime flag, set from config
static bool gGpuOverlayHasContent = false;       // Skip blend if nothing drawn this frame
static Rect gGpuOverlayDirtyRegion = { 0, 0, -1, -1 };  // Track dirty region

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
    bool integerScaling = settings.system.virtual_adapter;
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
        
        // Log render path configuration for debugging
        diagnosticsLog(
            DiagnosticsLevel::Info,
            "RENDERPATH",
            "CONFIG: virtual_adapter=%d virtual_adapter_fullres=%d render_display_orchestrator=%d render_path_trace=%d",
            settings.system.virtual_adapter ? 1 : 0,
            settings.system.virtual_adapter_fullres ? 1 : 0,
            settings.system.render_display_orchestrator ? 1 : 0,
            settings.debug.render_path_trace ? 1 : 0);
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
        // Phase 7: Configure FPS limiter based on settings
        if (settings.system.target_fps > 0) {
            sharedFpsLimiter.setFps(settings.system.target_fps);
        }
        
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

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
        if (!windowIsVirtualScreenEnabled()) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        }
        updateTexturePaletteRange(start, count, palette);

        // Phase 2: Emit palette effect command for orchestrator
        RenderCommandPaletteEffectPayload palettePayload;
        palettePayload.type = 2; // palette update
        palettePayload.param = static_cast<uint16_t>(start | (count << 8));
        renderCommandEmitPaletteEffect(palettePayload);

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
        if (!windowIsVirtualScreenEnabled()) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        }
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

    if (windowIsVirtualScreenEnabled()) {
        unsigned char* virtualBuffer = windowGetVirtualScreenBuffer();
        const int virtualPitch = windowGetVirtualScreenPitch();
        if (virtualBuffer != nullptr && virtualPitch > 0) {
            for (int row = 0; row < clippedHeight; row++) {
                const unsigned char* srcRow = srcStart + row * srcPitch;
                unsigned char* destRow = virtualBuffer + (destRect.top + row) * virtualPitch + destRect.left;
                memcpy(destRow, srcRow, clippedWidth);
            }

            if (virtualAdapterTraceChannelEnabled()) {
                char stage[96];
                std::snprintf(stage, sizeof(stage), "show_rect dst=(%d,%d %dx%d)", destRect.left, destRect.top, clippedWidth, clippedHeight);
                logVirtualAdapterSourceSamples(stage, srcStart, clippedWidth, clippedHeight, srcPitch);
            }

            windowVirtualScreenInvalidateRect(destRect);
            windowPresentVirtualScreen();
        }

        return;
    }

    unsigned char* surfacePixels = static_cast<unsigned char*>(gSdlSurface->pixels);
    blitBufferToBuffer(srcStart, clippedWidth, clippedHeight, srcPitch, surfacePixels + gSdlSurface->pitch * destRect.top + destRect.left, gSdlSurface->pitch);

    if (isFullResPresenterActive()) {
        Rect blitRect = destRect;
        blitIndexedRectToTexture(surfacePixels, gSdlSurface->pitch, blitRect);
        return;
    }

    SDL_Rect sdlRect;
    sdlRect.x = destRect.left;
    sdlRect.y = destRect.top;
    sdlRect.w = clippedWidth;
    sdlRect.h = clippedHeight;

    SDL_BlitSurface(gSdlSurface, &sdlRect, gSdlTextureSurface, &sdlRect);
}

void blitIndexedRectToTexture(const unsigned char* src, int srcPitch, const Rect& rect)
{
    // NOTE: We do NOT block indexed blits here. The indexed base layer must always
    // render to provide the background for UI, objects without HD assets, etc.
    // The HD overlay from the orchestrator is composited ON TOP of this base layer.
    // The "dual display" architecture means both layers render - indexed first, then HD overlay.

    if (src == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    // RENDER PATH TRACE: Log indexed path activity (informational, not an error)
    if (settings.debug.render_path_trace && settings.system.virtual_adapter) {
        bool orchestratorOwns = renderDisplayOrchestratorOwnsPresenter();
        diagnosticsLog(DiagnosticsLevel::Trace,  // Changed to Trace level - this is normal operation
            "RENDERPATH",
            "indexed_blit: rect=(%d,%d %dx%d) orchestrator_owns=%d",
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            orchestratorOwns ? 1 : 0);
    }

    // Note: Even in virtual adapter mode, the indexed base layer uploads
    // are required to populate the presenter with the 640x480 background.

    // Phase 6: Track direct write metric
    extern RenderCommandStats gRenderCommandStats;
    gRenderCommandStats.directWrites++;

    Rect logicalRect;
    Rect presenterRect;
    if (!resolvePresenterRect(rect, &logicalRect, &presenterRect)) {
        return;
    }

    const int width = rectGetWidth(&logicalRect);
    const int height = rectGetHeight(&logicalRect);
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

    const bool useScaledPresenter = isFullResPresenterActive();
    const DisplayScalerScaleTable* scaleTable = nullptr;
    const Rect* physicalViewport = nullptr;
    if (useScaledPresenter) {
        scaleTable = &displayScalerGetScaleTable();
        physicalViewport = &displayScalerGetPhysicalViewport();
    }

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
        const int logicalY = logicalRect.top + row;
        const unsigned char* srcRow = src + logicalY * srcPitch + logicalRect.left;

        uint32_t* linearDestRow = nullptr;
        int physicalRowStart = 0;
        int physicalRowEnd = -1;

        if (!useScaledPresenter) {
            linearDestRow = reinterpret_cast<uint32_t*>(destPixels + (presenterRect.top + row) * gSdlTextureSurface->pitch + presenterRect.left * bytesPerPixel);
        } else {
            physicalRowStart = scaleTable->vertical.starts[logicalY] - physicalViewport->top;
            physicalRowEnd = scaleTable->vertical.ends[logicalY] - physicalViewport->top;
            physicalRowStart = std::max(physicalRowStart, presenterRect.top);
            physicalRowEnd = std::min(physicalRowEnd, presenterRect.bottom);
            if (physicalRowStart > physicalRowEnd) {
                currentIndex += width;
                continue;
            }
        }

        for (int column = 0; column < width; column++) {
            const unsigned int paletteIndex = srcRow[column];
            const uint32_t mappedColor = gTexturePalette[paletteIndex];

            if (!useScaledPresenter) {
                linearDestRow[column] = mappedColor;
            } else {
                const int logicalX = logicalRect.left + column;
                int physicalColumnStart = scaleTable->horizontal.starts[logicalX] - physicalViewport->left;
                int physicalColumnEnd = scaleTable->horizontal.ends[logicalX] - physicalViewport->left;
                physicalColumnStart = std::max(physicalColumnStart, presenterRect.left);
                physicalColumnEnd = std::min(physicalColumnEnd, presenterRect.right);
                if (physicalColumnStart <= physicalColumnEnd) {
                    for (int physicalRow = physicalRowStart; physicalRow <= physicalRowEnd; physicalRow++) {
                        uint32_t* destRow = reinterpret_cast<uint32_t*>(destPixels + physicalRow * gSdlTextureSurface->pitch);
                        for (int physicalColumn = physicalColumnStart; physicalColumn <= physicalColumnEnd; physicalColumn++) {
                            destRow[physicalColumn] = mappedColor;
                        }
                    }
                }
            }

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
            logicalRect.left,
            logicalRect.top,
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
                logicalRect.left,
                logicalRect.top,
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

void clearPresenterRect(const Rect& rect)
{
    if (gSdlTextureSurface == nullptr) {
        return;
    }

    Rect logicalRect;
    Rect presenterRect;
    if (!resolvePresenterRect(rect, &logicalRect, &presenterRect)) {
        return;
    }

    const int width = rectGetWidth(&presenterRect);
    const int height = rectGetHeight(&presenterRect);
    if (width <= 0 || height <= 0) {
        return;
    }

    if (gSdlTextureSurface->format == nullptr || gSdlTextureSurface->format->BytesPerPixel != 4) {
        return;
    }

    unsigned char* destPixels = static_cast<unsigned char*>(gSdlTextureSurface->pixels);
    const int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;

    for (int row = 0; row < height; row++) {
        uint32_t* destRow = reinterpret_cast<uint32_t*>(destPixels + (presenterRect.top + row) * gSdlTextureSurface->pitch + presenterRect.left * bytesPerPixel);
        memset(destRow, 0, width * sizeof(uint32_t));
    }
}

int blitTrueColorRectToTexture(const uint32_t* src, const unsigned char* mask, int srcPitch, const Rect& rect)
{
    if (src == nullptr || gSdlTextureSurface == nullptr) {
        return 0;
    }

    Rect logicalRect;
    Rect presenterRect;
    if (!resolvePresenterRect(rect, &logicalRect, &presenterRect)) {
        return 0;
    }

    const int width = rectGetWidth(&logicalRect);
    const int height = rectGetHeight(&logicalRect);
    if (width <= 0 || height <= 0) {
        return 0;
    }

    if (gSdlTextureSurface->format == nullptr || gSdlTextureSurface->format->BytesPerPixel != 4) {
        return 0;
    }

    const int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    unsigned char* destPixels = static_cast<unsigned char*>(gSdlTextureSurface->pixels);
    int pixelsWritten = 0;

    const int deltaLeft = logicalRect.left - rect.left;
    const int deltaTop = logicalRect.top - rect.top;
    const uint32_t* logicalSrc = src + deltaTop * srcPitch + deltaLeft;
    const unsigned char* logicalMask = mask != nullptr ? mask + deltaTop * srcPitch + deltaLeft : nullptr;

    for (int row = 0; row < height; row++) {
        const uint32_t* srcRow = logicalSrc + row * srcPitch;
        const unsigned char* maskRow = logicalMask != nullptr ? logicalMask + row * srcPitch : nullptr;
        uint32_t* destRow = reinterpret_cast<uint32_t*>(destPixels + (presenterRect.top + row) * gSdlTextureSurface->pitch + presenterRect.left * bytesPerPixel);

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

int blitPhysicalTrueColorRectToTexture(const uint32_t* src, const unsigned char* mask, int srcPitch, const Rect& rect)
{
    if (src == nullptr || gSdlTextureSurface == nullptr) {
        return 0;
    }

    if (!isFullResPresenterActive()) {
        return 0;
    }

    Rect presenterBounds = getPresenterSurfaceBounds();
    Rect presenterRect;
    rectCopy(&presenterRect, &rect);
    if (rectIntersection(&presenterRect, &presenterBounds, &presenterRect) == -1) {
        return 0;
    }

    const int width = rectGetWidth(&presenterRect);
    const int height = rectGetHeight(&presenterRect);
    if (width <= 0 || height <= 0) {
        return 0;
    }

    if (gSdlTextureSurface->format == nullptr || gSdlTextureSurface->format->BytesPerPixel != 4) {
        return 0;
    }

    const int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    unsigned char* destPixels = static_cast<unsigned char*>(gSdlTextureSurface->pixels);
    const int deltaLeft = presenterRect.left - rect.left;
    const int deltaTop = presenterRect.top - rect.top;
    const uint32_t* presenterSrc = src + deltaTop * srcPitch + deltaLeft;
    const unsigned char* presenterMask = mask != nullptr ? mask + deltaTop * srcPitch + deltaLeft : nullptr;

    int pixelsWritten = 0;
    for (int row = 0; row < height; row++) {
        const uint32_t* srcRow = presenterSrc + row * srcPitch;
        const unsigned char* maskRow = presenterMask != nullptr ? presenterMask + row * srcPitch : nullptr;
        uint32_t* destRow = reinterpret_cast<uint32_t*>(destPixels + (presenterRect.top + row) * gSdlTextureSurface->pitch + presenterRect.left * bytesPerPixel);

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

// =============================================================================
// Phase 7: GPU Overlay Functions
// =============================================================================

bool gpuOverlayIsEnabled()
{
    return gGpuOverlayEnabled && gSdlOverlayTexture != nullptr;
}

int blitToGpuOverlayTexture(const uint32_t* src, int srcPitch, const Rect& rect)
{
    if (!gGpuOverlayEnabled || gSdlOverlayTexture == nullptr || src == nullptr) {
        return 0;
    }

    // Get presenter bounds for clipping
    Rect presenterBounds = getPresenterSurfaceBounds();
    Rect clipped;
    rectCopy(&clipped, &rect);
    if (rectIntersection(&clipped, &presenterBounds, &clipped) == -1) {
        return 0;
    }

    const int width = rectGetWidth(&clipped);
    const int height = rectGetHeight(&clipped);
    if (width <= 0 || height <= 0) {
        return 0;
    }

    // Lock only the dirty region of the GPU texture
    SDL_Rect sdlRect;
    sdlRect.x = clipped.left;
    sdlRect.y = clipped.top;
    sdlRect.w = width;
    sdlRect.h = height;

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(gSdlOverlayTexture, &sdlRect, &pixels, &pitch) != 0) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "GPU_OVERLAY",
                "failed to lock texture for blit: %s",
                SDL_GetError());
        }
        return 0;
    }

    // Calculate source offset if rect was clipped
    const int deltaLeft = clipped.left - rect.left;
    const int deltaTop = clipped.top - rect.top;
    const uint32_t* srcStart = src + deltaTop * srcPitch + deltaLeft;

    // Copy rows (memcpy per row is much faster than per-pixel loop)
    uint8_t* destRow = static_cast<uint8_t*>(pixels);
    for (int row = 0; row < height; row++) {
        const uint32_t* srcRow = srcStart + row * srcPitch;
        memcpy(destRow, srcRow, width * sizeof(uint32_t));
        destRow += pitch;
    }

    SDL_UnlockTexture(gSdlOverlayTexture);

    // Expand dirty region for this frame
    if (gGpuOverlayDirtyRegion.right < gGpuOverlayDirtyRegion.left) {
        // First dirty region this frame
        rectCopy(&gGpuOverlayDirtyRegion, &clipped);
    } else {
        // Union with existing dirty region
        if (clipped.left < gGpuOverlayDirtyRegion.left) gGpuOverlayDirtyRegion.left = clipped.left;
        if (clipped.top < gGpuOverlayDirtyRegion.top) gGpuOverlayDirtyRegion.top = clipped.top;
        if (clipped.right > gGpuOverlayDirtyRegion.right) gGpuOverlayDirtyRegion.right = clipped.right;
        if (clipped.bottom > gGpuOverlayDirtyRegion.bottom) gGpuOverlayDirtyRegion.bottom = clipped.bottom;
    }

    gGpuOverlayHasContent = true;
    return width * height;
}

void clearGpuOverlayRect(const Rect& rect)
{
    if (!gGpuOverlayEnabled || gSdlOverlayTexture == nullptr) {
        return;
    }

    Rect presenterBounds = getPresenterSurfaceBounds();
    Rect clipped;
    rectCopy(&clipped, &rect);
    if (rectIntersection(&clipped, &presenterBounds, &clipped) == -1) {
        return;
    }

    const int width = rectGetWidth(&clipped);
    const int height = rectGetHeight(&clipped);
    if (width <= 0 || height <= 0) {
        return;
    }

    // Lock only the region to clear
    SDL_Rect sdlRect;
    sdlRect.x = clipped.left;
    sdlRect.y = clipped.top;
    sdlRect.w = width;
    sdlRect.h = height;

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(gSdlOverlayTexture, &sdlRect, &pixels, &pitch) != 0) {
        return;
    }

    // Clear to transparent black (0x00000000)
    uint8_t* destRow = static_cast<uint8_t*>(pixels);
    for (int row = 0; row < height; row++) {
        memset(destRow, 0, width * sizeof(uint32_t));
        destRow += pitch;
    }

    SDL_UnlockTexture(gSdlOverlayTexture);
}

void scrollGpuOverlay(int dx, int dy)
{
    if (!gGpuOverlayEnabled || gSdlOverlayTexture == nullptr) {
        return;
    }

    if (dx == 0 && dy == 0) {
        return;
    }

    // For now, just clear the overlay on scroll
    // A more optimal implementation would use SDL_RenderCopy to shift content
    // but that requires a second texture and render-to-texture support
    
    SDL_SetRenderTarget(gSdlRenderer, gSdlOverlayTexture);
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 0);
    SDL_RenderClear(gSdlRenderer);
    SDL_SetRenderTarget(gSdlRenderer, nullptr);
    
    gGpuOverlayHasContent = false;
    gGpuOverlayDirtyRegion = { 0, 0, -1, -1 };
    
    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "GPU_OVERLAY",
            "scroll clear dx=%d dy=%d",
            dx, dy);
    }
}

void gpuOverlayResetForFrame()
{
    if (!gGpuOverlayEnabled || gSdlOverlayTexture == nullptr) {
        return;
    }

    // Clear the overlay texture for the new frame
    if (gGpuOverlayHasContent) {
        SDL_SetRenderTarget(gSdlRenderer, gSdlOverlayTexture);
        SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 0);  // Transparent black
        SDL_RenderClear(gSdlRenderer);
        SDL_SetRenderTarget(gSdlRenderer, nullptr);
    }

    gGpuOverlayHasContent = false;
    gGpuOverlayDirtyRegion = { 0, 0, -1, -1 };
}

// Clears drawing surface.
//
// 0x4CBBC8
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
        return;
    }

    if (windowIsVirtualScreenEnabled()) {
        unsigned char* virtualBuffer = windowGetVirtualScreenBuffer();
        int virtualPitch = windowGetVirtualScreenPitch();
        if (virtualBuffer != nullptr && virtualPitch > 0) {
            const Rect& logicalBounds = displayScalerGetLogicalBounds();
            const int width = rectGetWidth(&logicalBounds);
            const int height = rectGetHeight(&logicalBounds);
            for (int y = 0; y < height; y++) {
                memset(virtualBuffer + y * virtualPitch, 0, width);
            }

            Rect refreshRect = logicalBounds;
            // Repaint the entire GNW stack before presenting cleared memory.
            windowRefreshAll(&refreshRect);

            renderCommandEmitViewportEvent(RenderViewportEventType::Blackout,
                logicalBounds,
                0,
                0,
                windowVirtualScreenGetDirtySequence());
        }

        return;
    }

    if (gSdlSurface == nullptr || gSdlTextureSurface == nullptr) {
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
    // Phase 7: Enable VSync if configured for tear-free rendering
    Uint32 rendererFlags = 0;
    if (settings.system.vsync) {
        rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
    }
    
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, rendererFlags);
    if (gSdlRenderer == nullptr) {
        return false;
    }
    
    // Log renderer info
    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(gSdlRenderer, &info) == 0) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "RENDERER",
                "using %s, vsync=%s, flags=0x%x",
                info.name,
                (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "on" : "off",
                info.flags);
        }
    }

    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    int presenterWidth = logicalSpace.width;
    int presenterHeight = logicalSpace.height;

    if (isFullResPresenterActive()) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        presenterWidth = std::max(1, rectGetWidth(&viewport));
        presenterHeight = std::max(1, rectGetHeight(&viewport));
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, presenterWidth, presenterHeight);
    if (gSdlTexture == nullptr) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, presenterWidth, presenterHeight, SDL_BITSPERPIXEL(format), format);
        // Phase 6 FIX: Clear texture to black to prevent uninitialized memory artifacts
        // (triangular pattern garbage visible during screen transitions)
        SDL_SetRenderTarget(gSdlRenderer, gSdlTexture);
        SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
        SDL_RenderClear(gSdlRenderer);
        SDL_SetRenderTarget(gSdlRenderer, nullptr);

    if (gSdlTextureSurface == nullptr) {
        return false;
    }

    // Phase 7: Create GPU overlay texture for HD content
    // Only create if virtual adapter with full-res is active
    if (isFullResPresenterActive() && settings.system.gpu_overlay) {
        gSdlOverlayTexture = SDL_CreateTexture(
            gSdlRenderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            presenterWidth,
            presenterHeight
        );
        
        if (gSdlOverlayTexture != nullptr) {
            // Enable alpha blending for overlay compositing
            SDL_SetTextureBlendMode(gSdlOverlayTexture, SDL_BLENDMODE_BLEND);
            
            // Clear to transparent black
            SDL_SetRenderTarget(gSdlRenderer, gSdlOverlayTexture);
            SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 0);
            SDL_RenderClear(gSdlRenderer);
            SDL_SetRenderTarget(gSdlRenderer, nullptr);
            
            gGpuOverlayEnabled = true;
            gGpuOverlayHasContent = false;
            gGpuOverlayDirtyRegion = { 0, 0, -1, -1 };
            
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "created overlay texture %dx%d (STREAMING, BLEND)",
                    presenterWidth,
                    presenterHeight);
            }
        } else {
            // Fallback to CPU path if GPU texture creation fails
            gGpuOverlayEnabled = false;
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "failed to create overlay texture, falling back to CPU path: %s",
                    SDL_GetError());
            }
        }
    } else {
        gGpuOverlayEnabled = false;
    }

    return true;
}

static void destroyRenderer()
{
    // Phase 7: Destroy GPU overlay texture
    if (gSdlOverlayTexture != nullptr) {
        SDL_DestroyTexture(gSdlOverlayTexture);
        gSdlOverlayTexture = nullptr;
        gGpuOverlayEnabled = false;
        gGpuOverlayHasContent = false;
    }

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

    logPresenterSurfaceState("renderer_destroy", false, 0, 0);
}

static void logPresenterSurfaceState(const char* reason, bool fullRes, int width, int height)
{
    gPresenterSurfaceFullRes = fullRes;
    gPresenterSurfaceWidth = width;
    gPresenterSurfaceHeight = height;

    if (!diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        return;
    }

    if (gPresenterSurfaceLogInitialized
        && gPresenterSurfaceLastFullRes == fullRes
        && gPresenterSurfaceLastWidth == width
        && gPresenterSurfaceLastHeight == height) {
        return;
    }

    gPresenterSurfaceLogInitialized = true;
    gPresenterSurfaceLastFullRes = fullRes;
    gPresenterSurfaceLastWidth = width;
    gPresenterSurfaceLastHeight = height;

    diagnosticsLog(DiagnosticsLevel::Info,
        "RENDERER",
        "presenter surface mode=%s size=%dx%d reason=%s",
        fullRes ? "fullres" : "logical",
        width,
        height,
        reason != nullptr ? reason : "update");
}

static bool ensurePresenterSurfaceMatchesBounds()
{
    if (gSdlRenderer == nullptr) {
        logPresenterSurfaceState("renderer_missing", false, 0, 0);
        return false;
    }

    bool wantFullResPresenter = false;
    int desiredWidth = 0;
    int desiredHeight = 0;
    const char* stateReason = nullptr;

    if (settings.system.virtual_adapter && settings.system.virtual_adapter_fullres) {
        const double scale = displayScalerGetScale();
        if (scale >= 1.0 - 1.0e-4) {
            const Rect& viewport = displayScalerGetPhysicalViewport();
            desiredWidth = rectGetWidth(&viewport);
            desiredHeight = rectGetHeight(&viewport);
            wantFullResPresenter = desiredWidth > 0 && desiredHeight > 0;
            if (wantFullResPresenter) {
                stateReason = "fullres_requested";
            }
        } else {
            stateReason = "scale_below_one";
        }
    }

    if (!wantFullResPresenter) {
        const Rect& logicalBounds = displayScalerGetLogicalBounds();
        desiredWidth = rectGetWidth(&logicalBounds);
        desiredHeight = rectGetHeight(&logicalBounds);
        if (stateReason == nullptr) {
            stateReason = settings.system.virtual_adapter_fullres ? "viewport_invalid" : "fullres_disabled";
        }
    }

    if (desiredWidth <= 0 || desiredHeight <= 0) {
        logPresenterSurfaceState("invalid_dimensions", false, desiredWidth, desiredHeight);
        return false;
    }

    const int currentWidth = gSdlTextureSurface != nullptr ? gSdlTextureSurface->w : 0;
    const int currentHeight = gSdlTextureSurface != nullptr ? gSdlTextureSurface->h : 0;
    if (gSdlTexture != nullptr && gSdlTextureSurface != nullptr && currentWidth == desiredWidth && currentHeight == desiredHeight) {
        logPresenterSurfaceState(wantFullResPresenter ? "fullres_ready" : stateReason, wantFullResPresenter, desiredWidth, desiredHeight);
        return true;
    }

    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, desiredWidth, desiredHeight);
    if (newTexture == nullptr) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "RENDERER",
                "failed to resize presenter texture %dx%d: %s",
                desiredWidth,
                desiredHeight,
                SDL_GetError());
        }
        logPresenterSurfaceState("texture_alloc_failed", false, currentWidth, currentHeight);
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(newTexture, &format, nullptr, nullptr, nullptr) != 0) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "RENDERER",
                "SDL_QueryTexture failed during presenter resize: %s",
                SDL_GetError());
        }
        SDL_DestroyTexture(newTexture);
        logPresenterSurfaceState("query_failed", false, currentWidth, currentHeight);
        return false;
    }

    SDL_Surface* newSurface = SDL_CreateRGBSurfaceWithFormat(0, desiredWidth, desiredHeight, SDL_BITSPERPIXEL(format), format);
    if (newSurface == nullptr) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "RENDERER",
                "failed to allocate presenter surface %dx%d: %s",
                desiredWidth,
                desiredHeight,
                SDL_GetError());
        }
        SDL_DestroyTexture(newTexture);
            logPresenterSurfaceState("surface_alloc_failed", false, currentWidth, currentHeight);
        return false;
    }

    SDL_FillRect(newSurface, nullptr, 0);

    if (gSdlTextureSurface != nullptr) {
        SDL_FreeSurface(gSdlTextureSurface);
    }

    if (gSdlTexture != nullptr) {
        SDL_DestroyTexture(gSdlTexture);
    }

    gSdlTexture = newTexture;
    gSdlTextureSurface = newSurface;
    // Phase 6 FIX: Clear texture to black to prevent uninitialized memory artifacts
    SDL_SetRenderTarget(gSdlRenderer, gSdlTexture);
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gSdlRenderer);
    SDL_SetRenderTarget(gSdlRenderer, nullptr);

    // Phase 7: Recreate GPU overlay texture to match new presenter size
    if (gSdlOverlayTexture != nullptr) {
        SDL_DestroyTexture(gSdlOverlayTexture);
        gSdlOverlayTexture = nullptr;
        gGpuOverlayEnabled = false;
    }
    
    if (wantFullResPresenter && settings.system.gpu_overlay) {
        gSdlOverlayTexture = SDL_CreateTexture(
            gSdlRenderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            desiredWidth,
            desiredHeight
        );
        
        if (gSdlOverlayTexture != nullptr) {
            SDL_SetTextureBlendMode(gSdlOverlayTexture, SDL_BLENDMODE_BLEND);
            
            SDL_SetRenderTarget(gSdlRenderer, gSdlOverlayTexture);
            SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 0);
            SDL_RenderClear(gSdlRenderer);
            SDL_SetRenderTarget(gSdlRenderer, nullptr);
            
            gGpuOverlayEnabled = true;
            gGpuOverlayHasContent = false;
            gGpuOverlayDirtyRegion = { 0, 0, -1, -1 };
            
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "resized overlay texture to %dx%d",
                    desiredWidth,
                    desiredHeight);
            }
        } else {
            gGpuOverlayEnabled = false;
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "failed to resize overlay texture: %s",
                    SDL_GetError());
            }
        }
    }

    windowVirtualScreenInvalidateAll();

    logPresenterSurfaceState(wantFullResPresenter ? "resized_fullres" : "resized_logical", wantFullResPresenter, desiredWidth, desiredHeight);

    return true;
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
    windowRefreshPhysicalTrueColorBuffers();

    destroyRenderer();

    const Rect& logical = displayScalerGetLogicalBounds();
    rectCopy(&_scr_size, &logical);

    createRenderer();

    if (gSdlTextureSurface != nullptr && gSdlSurface != nullptr) {
        if (!windowIsVirtualScreenEnabled()) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        } else {
            windowVirtualScreenInvalidateAll();
            windowPresentVirtualScreen();

            renderCommandEmitViewportEvent(RenderViewportEventType::Resize,
                logical,
                0,
                0,
                windowVirtualScreenGetDirtySequence());
        }
    }

    syncPhysicalSizeWithRenderer();
}

void renderPresent()
{
    syncPhysicalSizeWithRenderer();
    ensurePresenterSurfaceMatchesBounds();
    renderTraceCommitFrame();
    windowPresentVirtualScreen();
    renderCommandsBeforePresent();

    Rect presenterBounds = getPresenterSurfaceBounds();
    const int presenterWidth = rectGetWidth(&presenterBounds);
    const int presenterHeight = rectGetHeight(&presenterBounds);
    SDL_Rect srcRect;
    srcRect.x = 0;
    srcRect.y = 0;
    srcRect.w = presenterWidth;
    srcRect.h = presenterHeight;

    Rect uploadRect = presenterBounds;

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
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "SDL_UpdateTexture fallback copy succeeded for %dx%d", presenterWidth, presenterHeight);
            }
        } else if (gTextureUploadFallbackLogBudget > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "SDL_UpdateTexture fallback copy failed for %dx%d", presenterWidth, presenterHeight);
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

    // Render the base indexed layer
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, &srcRect, &destRect);
    
    // Phase 7: Composite GPU overlay on top if enabled and has content
    if (gGpuOverlayEnabled && gGpuOverlayHasContent && gSdlOverlayTexture != nullptr) {
        SDL_RenderCopy(gSdlRenderer, gSdlOverlayTexture, &srcRect, &destRect);
        
        if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
            diagnosticsLog(DiagnosticsLevel::Trace,
                "GPU_OVERLAY",
                "composited overlay dirty=(%d,%d %dx%d)",
                gGpuOverlayDirtyRegion.left,
                gGpuOverlayDirtyRegion.top,
                rectGetWidth(&gGpuOverlayDirtyRegion),
                rectGetHeight(&gGpuOverlayDirtyRegion));
        }
    }
    
    virtualInputRenderOverlay(gSdlRenderer);
    SDL_RenderPresent(gSdlRenderer);

    // Phase 6: Log frame metrics periodically (every 300 frames = ~5 seconds at 60fps)
    static int frameCounter = 0;
    if (++frameCounter >= 300) {
        renderCommandLogFrameMetrics();
        frameCounter = 0;
    }
}

} // namespace fallout
