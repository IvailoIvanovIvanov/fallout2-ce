#include "svga.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits.h>
#include <string.h>

#include <SDL.h>

#include "color.h"
#include "config.h"
#include "diagnostics.h"
#include "display_scaler.h"
#include "draw.h"
#include "geometry.h"
#include "gpu_device.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "render_trace.h"
#include "render_commands.h"
#include "render_display_orchestrator.h"
#include "settings.h"
#include "upscaler.h"
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
static Rect getGpuOverlayBounds();  // Phase 8.3: Overlay uses physical viewport size
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

// Phase 8.5: Streaming texture upload using SDL_LockTexture
// This provides direct GPU memory access, avoiding the extra copy in SDL_UpdateTexture
// For STREAMING textures, this is the optimal upload path
static bool streamingSurfaceRectToTexture(SDL_Texture* texture, SDL_Surface* surface, const Rect& rect)
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
    const int rowBytes = width * bytesPerPixel;
    uint8_t* destPixels = static_cast<uint8_t*>(destination);
    
    // Optimized row copy - if pitches match and we're copying full width, use single memcpy
    if (sourcePitch == destinationPitch && clipped.left == 0 && width == surface->w) {
        // Single contiguous copy for full-width regions
        std::memcpy(destPixels, sourcePixels + clipped.top * sourcePitch, height * sourcePitch);
    } else {
        // Row-by-row copy for partial regions
        for (int row = 0; row < height; row++) {
            const uint8_t* sourceRow = sourcePixels + (clipped.top + row) * sourcePitch + clipped.left * bytesPerPixel;
            std::memcpy(destPixels + row * destinationPitch, sourceRow, rowBytes);
        }
    }

    SDL_UnlockTexture(texture);
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
    return false;  // virtual_adapter_trace removed
    
    // Original code preserved but unreachable:
    /*
    if (!settings.debug.virtual_adapter_trace) {
        return false;
    }

    if (!windowIsVirtualScreenEnabled()) {
        return false;
    }

    return diagnosticsWouldLog(DiagnosticsLevel::Trace);
    */
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

// Phase 8.3: Check if GPU scaling is enabled
// When true, the indexed layer stays at logical resolution and GPU scales during render
static bool isGpuScalingActive()
{
    return settings.system.gpu_scaling;  // virtual_adapter removed
}

static bool isFullResPresenterActive()
{
    // Phase 8.3: If GPU scaling is enabled, never use full-res presenter for indexed layer
    // This keeps the texture at 640x480 and lets the GPU scale to physical resolution
    if (isGpuScalingActive()) {
        return false;
    }
    
    if (true) {  // virtual_adapter removed
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

// Phase 8.3: GPU overlay always uses physical viewport size
// This is different from presenter bounds when gpu_scaling is enabled
static Rect getGpuOverlayBounds()
{
    Rect bounds = { 0, 0, -1, -1 };
    
    // Overlay texture is always at physical resolution
    const Rect& viewport = displayScalerGetPhysicalViewport();
    int width = rectGetWidth(&viewport);
    int height = rectGetHeight(&viewport);
    if (width > 0 && height > 0) {
        bounds.right = width - 1;
        bounds.bottom = height - 1;
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
static int gOverlayTextureWidth = 0;             // Phase 8.3 fix: Track overlay dimensions
static int gOverlayTextureHeight = 0;            // to detect when resize is needed

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
    bool integerScaling = false;  // virtual_adapter removed
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
            "CONFIG: render_display_orchestrator=%d render_path_trace=%d",  // virtual_adapter removed
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
        // Phase 7c: VSync awareness - when VSync is on, FPS limiter skips throttling
        sharedFpsLimiter.setVSyncEnabled(settings.system.vsync);
        
        // Phase 7d: Enable Direct3D 12 rendering on Windows for GPU compute access
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d12");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

        Uint32 windowFlags = SDL_WINDOW_ALLOW_HIGHDPI;

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
        
        // Initialize upscaler with game resolution
        // Input: 640x480 (classic Fallout 2 resolution)
        // Output: Physical display resolution
        // For integer scaling modes, the upscaler will center content with letterboxing
        diagnosticsLog(DiagnosticsLevel::Info, "SVGA", "Initializing upscaler: 640x480 -> %dx%d", 
                      physicalWidth, physicalHeight);
        if (upscalerInit(640, 480, physicalWidth, physicalHeight, UpscalerMode::INTEGER_3X) != 0) {
            diagnosticsLog(DiagnosticsLevel::Info, "SVGA", "Upscaler initialization failed, continuing without upscaling");
        } else {
            // Upscaler initialized successfully!
            // CRITICAL: Recreate the GPU texture with physical dimensions if upscaler is active.
            // The initial texture was created at logical resolution (640x480) because upscaler wasn't ready yet.
            if (upscalerIsAvailable()) {
                diagnosticsLog(DiagnosticsLevel::Info, "SVGA", "Upscaler active - recreating texture at physical resolution: %dx%d", 
                              physicalWidth, physicalHeight);
                
                if (gSdlTexture != nullptr) {
                    SDL_DestroyTexture(gSdlTexture);
                }
                
                gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 
                                               physicalWidth, physicalHeight);
                                               
                if (gSdlTexture != nullptr) {
                    // Clear to black to avoid garbage
                    SDL_SetRenderTarget(gSdlRenderer, gSdlTexture);
                    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
                    SDL_RenderClear(gSdlRenderer);
                    SDL_SetRenderTarget(gSdlRenderer, nullptr);
                } else {
                    diagnosticsLog(DiagnosticsLevel::Info, "SVGA", "Failed to recreate texture: %s", SDL_GetError());
                }
            }
        }

        // CRITICAL FIX: Check if GPU device was lost during upscaler initialization (TDR)
        // If the GPU device is gone, the SDL renderer (which shares the adapter) is likely invalid.
        // We must recreate the renderer to recover from the driver reset.
        if (!gpuDeviceIsReady()) {
            diagnosticsLog(DiagnosticsLevel::Info, "SVGA", "GPU device not ready after upscaler init - checking for renderer recovery");
            
            // Force fallback to D3D11 to ensure stability if D3D12 is unstable/crashed
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
            
            destroyRenderer();
            if (!createRenderer()) {
                destroyRenderer();
                SDL_DestroyWindow(gSdlWindow);
                gSdlWindow = nullptr;
                return -1;
            }
            syncPhysicalSizeWithRenderer();
            diagnosticsLog(DiagnosticsLevel::Info, "SVGA", "Renderer recreated successfully (fallback mode)");
        }
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
            // Phase 7c: Only present immediately if deferred presentation is disabled
            if (!windowIsDeferredPresentationEnabled()) {
                windowPresentVirtualScreen();
            }
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
    if (false) {  // virtual_adapter removed
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

    // Phase 8.3: Use overlay bounds (physical resolution) not presenter bounds
    Rect overlayBounds = getGpuOverlayBounds();
    Rect clipped;
    rectCopy(&clipped, &rect);
    if (rectIntersection(&clipped, &overlayBounds, &clipped) == -1) {
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

    // Phase 8.3: Use overlay bounds (physical resolution) not presenter bounds
    Rect overlayBounds = getGpuOverlayBounds();
    Rect clipped;
    rectCopy(&clipped, &rect);
    if (rectIntersection(&clipped, &overlayBounds, &clipped) == -1) {
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

    // Phase 7c: Tell FPS limiter whether VSync is active
    // When VSync is on, the GPU already limits frame rate so FPS limiter can skip throttling
    sharedFpsLimiter.setVSyncEnabled(settings.system.vsync);

    LogicalSpace logicalSpace = displayScalerGetLogicalSpace();
    int presenterWidth = logicalSpace.width;
    int presenterHeight = logicalSpace.height;

    if (isFullResPresenterActive()) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        presenterWidth = std::max(1, rectGetWidth(&viewport));
        presenterHeight = std::max(1, rectGetHeight(&viewport));
    }
    
    // ========================================================================
    // GPU TEXTURE CREATION - DYNAMIC SIZING
    // ========================================================================
    // The GPU texture (gSdlTexture) holds the final frame data before rendering.
    // Its size depends on whether the upscaler is active:
    //
    // WITHOUT UPSCALER:
    // - Texture size: 640×480 (logical/presenter size)
    // - Content: Direct upload from gSdlTextureSurface (converted phantom display)
    // - SDL stretches 640×480 texture to viewport during rendering
    //
    // WITH UPSCALER (INTEGER_3X):
    // - Texture size: 2560×1440 (physical display size)
    // - Content: Upscaled + letterboxed output from upscaler
    // - Example: 1920×1440 scaled content centered with 320px black bars
    // - SDL renders texture 1:1 to display (no stretching needed)
    //
    // This dynamic sizing ensures optimal quality:
    // - Without upscaler: Standard SDL scaling handles resolution difference
    // - With upscaler: Pre-scaled content rendered directly (perfect pixels)
    // ========================================================================
    int textureWidth = presenterWidth;
    int textureHeight = presenterHeight;
    if (upscalerIsAvailable()) {
        int physicalWidth, physicalHeight;
        SDL_GetWindowSize(gSdlWindow, &physicalWidth, &physicalHeight);
        textureWidth = physicalWidth;
        textureHeight = physicalHeight;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, textureWidth, textureHeight);
    if (gSdlTexture == nullptr) {
        return false;
    }

    // Ensure the main presenter texture is treated as fully opaque.
    // If blending is enabled and alpha is <255 for any pixels, SDL/DWM composition can
    // produce lifted blacks and washed-out colors (very noticeable on OLED).
    SDL_SetTextureBlendMode(gSdlTexture, SDL_BLENDMODE_NONE);
    SDL_SetTextureAlphaMod(gSdlTexture, 255);
    SDL_SetTextureColorMod(gSdlTexture, 255, 255, 255);

    // Also disable renderer blending for the base pass.
    SDL_SetRenderDrawBlendMode(gSdlRenderer, SDL_BLENDMODE_NONE);
    
    // DEBUG: Log texture creation
    FILE* pipelineLog = fopen("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 2\\pipeline.log", "w");
    if (pipelineLog) {
        fprintf(pipelineLog, "[TEXTURE CREATE] Logical space: %dx%d\n", logicalSpace.width, logicalSpace.height);
        fprintf(pipelineLog, "[TEXTURE CREATE] Presenter: %dx%d\n", presenterWidth, presenterHeight);
        fprintf(pipelineLog, "[TEXTURE CREATE] Upscaler available: %s\n", upscalerIsAvailable() ? "YES" : "NO");
        fprintf(pipelineLog, "[TEXTURE CREATE] Texture created: %dx%d\n", textureWidth, textureHeight);
        fclose(pipelineLog);
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

    // Phase 7/8.3: Create GPU overlay texture for HD content
    // The overlay always needs physical resolution for HD assets, regardless of base layer scaling
    // When gpu_scaling is enabled, base layer is 640x480 but overlay is still physical res
    const bool needsHdOverlay = false;  // virtual_adapter removed
    if (needsHdOverlay) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        const int overlayWidth = std::max(1, rectGetWidth(&viewport));
        const int overlayHeight = std::max(1, rectGetHeight(&viewport));
        
        gSdlOverlayTexture = SDL_CreateTexture(
            gSdlRenderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            overlayWidth,
            overlayHeight
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
            gOverlayTextureWidth = overlayWidth;   // Track size for resize detection
            gOverlayTextureHeight = overlayHeight;
            
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "created overlay texture %dx%d (STREAMING, BLEND), base=%dx%d gpu_scaling=%d",
                    overlayWidth,
                    overlayHeight,
                    presenterWidth,
                    presenterHeight,
                    isGpuScalingActive() ? 1 : 0);
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

    // Phase 8: Initialize GPU device for compute operations (e.g., AI upscaling)
    // GPU device needed for:
    // - gpu_scaling (GPU texture acceleration)
    // - virtual_adapter (virtual display adapter)
    // - ANIME4K upscaler (mode 5) - requires GPU compute
    int upscalerMode = 3; // Default INTEGER_3X
    configGetInt(&gGameConfig, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_UPSCALER_MODE_KEY, &upscalerMode);
    bool needsGpuCompute = (upscalerMode == 5); // ANIME4K
    
    if (settings.system.gpu_scaling || needsGpuCompute) {  // virtual_adapter removed
        if (!gpuDeviceInit()) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER",
                "GPU device initialization failed, GPU compute operations unavailable");
            if (needsGpuCompute) {
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER",
                    "WARNING: Upscaler mode %d requires GPU compute but initialization failed!", upscalerMode);
            }
        } else if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER",
                "GPU device initialized for compute operations");
        }
    }

    return true;
}

static void destroyRenderer()
{
    // Phase 8: Shutdown GPU device
    gpuDeviceShutdown();

    // Phase 7: Destroy GPU overlay texture
    if (gSdlOverlayTexture != nullptr) {
        SDL_DestroyTexture(gSdlOverlayTexture);
        gSdlOverlayTexture = nullptr;
        gGpuOverlayEnabled = false;
        gGpuOverlayHasContent = false;
        gOverlayTextureWidth = 0;
        gOverlayTextureHeight = 0;
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

// ============================================================================
// PRESENTER SURFACE MANAGEMENT
// ============================================================================
// ensurePresenterSurfaceMatchesBounds() - Ensure texture surface matches logical space
//
// The "presenter surface" (gSdlTextureSurface) is an intermediate RGBA surface
// that bridges the phantom display (indexed color) and the GPU texture.
//
// SURFACE HIERARCHY:
// 1. gSdlSurface (phantom display): 640×480 indexed color (game renders here)
// 2. gSdlTextureSurface (presenter): 640×480 RGBA converted (uploaded to GPU)
// 3. gSdlTexture (GPU): Variable size (640×480 or 2560×1440 depending on upscaler)
//
// This function ensures gSdlTextureSurface matches the logical space dimensions
// (typically 640×480). If dimensions change or surface doesn't exist, it's
// recreated with the correct size.
//
// RETURNS: true on success, false if surface creation fails
// ============================================================================
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

    // Phase 8.3: If GPU scaling is enabled, always use logical resolution for base layer
    // The GPU will scale during SDL_RenderCopy, which is much faster than CPU scaling
    if (isGpuScalingActive()) {
        const Rect& logicalBounds = displayScalerGetLogicalBounds();
        desiredWidth = rectGetWidth(&logicalBounds);
        desiredHeight = rectGetHeight(&logicalBounds);
        stateReason = "gpu_scaling_active";
    } else if (false) {  // virtual_adapter removed
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

    if (!wantFullResPresenter && desiredWidth <= 0) {
        const Rect& logicalBounds = displayScalerGetLogicalBounds();
        desiredWidth = rectGetWidth(&logicalBounds);
        desiredHeight = rectGetHeight(&logicalBounds);
        if (stateReason == nullptr) {
            stateReason = "fullres_disabled";  // virtual_adapter removed
        }
    }

    if (desiredWidth <= 0 || desiredHeight <= 0) {
        logPresenterSurfaceState("invalid_dimensions", false, desiredWidth, desiredHeight);
        return false;
    }

    const int currentWidth = gSdlTextureSurface != nullptr ? gSdlTextureSurface->w : 0;
    const int currentHeight = gSdlTextureSurface != nullptr ? gSdlTextureSurface->h : 0;
    
    // Phase 8.3 fix: Also check if overlay needs recreation
    // When gpu_scaling is enabled, base texture is 640x480 but overlay needs physical resolution
    bool overlayNeedsResize = false;
    if (false) {  // virtual_adapter removed
        const Rect& viewport = displayScalerGetPhysicalViewport();
        const int neededOverlayWidth = std::max(1, rectGetWidth(&viewport));
        const int neededOverlayHeight = std::max(1, rectGetHeight(&viewport));
        if (gOverlayTextureWidth != neededOverlayWidth || gOverlayTextureHeight != neededOverlayHeight) {
            overlayNeedsResize = true;
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "overlay resize needed: current=%dx%d needed=%dx%d",
                    gOverlayTextureWidth, gOverlayTextureHeight,
                    neededOverlayWidth, neededOverlayHeight);
            }
        }
    }
    
    if (gSdlTexture != nullptr && gSdlTextureSurface != nullptr && currentWidth == desiredWidth && currentHeight == desiredHeight && !overlayNeedsResize) {
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

    // Phase 7/8.3: Recreate GPU overlay texture
    // Overlay always needs physical resolution for HD assets, even when base uses gpu_scaling
    if (gSdlOverlayTexture != nullptr) {
        SDL_DestroyTexture(gSdlOverlayTexture);
        gSdlOverlayTexture = nullptr;
        gGpuOverlayEnabled = false;
        gOverlayTextureWidth = 0;
        gOverlayTextureHeight = 0;
    }
    
    const bool needsHdOverlay = false;  // virtual_adapter removed
    if (needsHdOverlay) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        const int overlayWidth = std::max(1, rectGetWidth(&viewport));
        const int overlayHeight = std::max(1, rectGetHeight(&viewport));
        
        gSdlOverlayTexture = SDL_CreateTexture(
            gSdlRenderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            overlayWidth,
            overlayHeight
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
            gOverlayTextureWidth = overlayWidth;   // Track size for resize detection
            gOverlayTextureHeight = overlayHeight;
            
            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "GPU_OVERLAY",
                    "resized overlay texture to %dx%d (base=%dx%d gpu_scaling=%d)",
                    overlayWidth,
                    overlayHeight,
                    desiredWidth,
                    desiredHeight,
                    isGpuScalingActive() ? 1 : 0);
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

// ============================================================================
// RENDERING PIPELINE - MAIN ENTRY POINT
// ============================================================================
// renderPresent() - Main frame rendering pipeline
//
// This is the core rendering function that processes each frame through the
// complete pipeline from the "phantom display" to the physical screen.
//
// PIPELINE OVERVIEW:
// 1. Phantom Display (gSdlSurface): 640x480 indexed color surface where the
//    game renders. This is Fallout 2's native resolution and color depth.
//
// 2. Upscaler Processing (optional): If upscaler is available, convert indexed
//    colors to RGBA, apply filters (Kuwahara), then scale (INTEGER_3X) or
//    upscale (Anime4K). Output is typically 2560x1440 for modern displays.
//
// 3. Texture Upload: Upload either the upscaled content or original phantom
//    display content to GPU texture (gSdlTexture).
//
// 4. Rendering: Render the texture to screen with proper aspect ratio and
//    letterboxing (black bars) to maintain 4:3 aspect.
//
// KEY SURFACES:
// - gSdlSurface: 640x480 indexed ("phantom display") - game renders here
// - gSdlTextureSurface: 640x480 RGBA converted from phantom display
// - gSdlTexture: Variable size (640x480 or 2560x1440) GPU texture for rendering
//
// UPSCALER MODES:
// - INTEGER_2X/3X/4X: Perfect pixel replication with optional Kuwahara filter
// - ANIME4K: Shader-based upscaling
// ============================================================================
void renderPresent()
{
    static int renderPresentCallCount = 0;
    renderPresentCallCount++;
    
    syncPhysicalSizeWithRenderer();
    ensurePresenterSurfaceMatchesBounds();
    renderTraceCommitFrame();
    windowPresentVirtualScreen();
    renderCommandsBeforePresent();
    
    // ========================================================================
    // UPSCALER PROCESSING PIPELINE
    // ========================================================================
    // This section handles optional upscaling before texture upload.
    // If upscaler is available and configured, it processes the phantom display
    // through the complete pipeline: palette conversion → filters → scaling
    //
    // PROCESSING STEPS:
    // 1. Extract game buffer from phantom display (gSdlSurface->pixels)
    // 2. Extract palette from SDL surface format (256 RGBA colors)
    // 3. Upload indexed data + palette to upscaler (upscalerSetIndexedInput)
    // 4. Execute upscaling pipeline (upscalerDispatch):
    //    - Convert indexed → RGBA (640×480×4 bytes)
    //    - Apply Kuwahara filter (optional edge-preserving smoothing)
    //    - Integer scale with letterboxing (e.g., 3× → 1920×1440 centered in 2560×1440)
    // 5. Retrieve upscaled output buffer (upscalerGetOutputBuffer)
    // 6. Upload to GPU texture (gSdlTexture)
    //
    // If upscaler is not available or disabled, skip to standard texture upload
    // (direct conversion of phantom display → texture via gSdlTextureSurface)
    // ========================================================================
    
    // Check if upscaler is available
    bool available = upscalerIsAvailable();
    bool upscalerDidRender = false;  // Track if upscaler handled the frame
    int actualUpscaledWidth = 0;  // Actual content dimensions from upscaler
    int actualUpscaledHeight = 0;
    
    if (available) {
        // STEP 1: Get game buffer from phantom display (640×480 indexed color)
        // This is where Fallout 2 actually renders each frame
        unsigned char* gameBuffer = nullptr;
        if (gSdlSurface != nullptr && gSdlSurface->pixels != nullptr) {
            gameBuffer = static_cast<unsigned char*>(gSdlSurface->pixels);
        }
        
        // DEBUG: Log only once on frame 1 (avoid repeated file I/O)
        static bool bufferLogged = false;
        if (!bufferLogged) {
            bufferLogged = true;
            FILE* debugLog = fopen("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 2\\upscale.log", "a");
            if (debugLog) {
                fprintf(debugLog, "[RENDER] Frame 1: gSdlSurface=%p, gameBuffer=%p, available=%d\n", 
                    gSdlSurface, gameBuffer, available ? 1 : 0);
                fflush(debugLog);
                fclose(debugLog);
            }
        }
        
        if (gameBuffer != nullptr) {
            // Get current palette from SDL surface format
            unsigned char* paletteData = nullptr;
            if (gSdlSurface->format->palette != nullptr) {
                paletteData = reinterpret_cast<unsigned char*>(gSdlSurface->format->palette->colors);
            }
            
            if (paletteData != nullptr) {
                // Convert palette from SDL_Color (RGB888) to ARGB8888 format
                uint32_t palette[256];
                for (int i = 0; i < 256; i++) {
                    SDL_Color* color = &gSdlSurface->format->palette->colors[i];
                    palette[i] = 0xFF000000 |  // Alpha = 255
                                (color->r << 16) |  // R
                                (color->g << 8) |   // G
                                (color->b);         // B
                }
                
                // Set indexed input (converts to RGBA internally)
                int setInputResult = upscalerSetIndexedInput(gameBuffer, palette);
                
                if (setInputResult == 0) {
                    // Dispatch upscaling (Kuwahara + integer scale or Anime4K)
                    int dispatchResult = upscalerDispatch();
                    
                    if (dispatchResult == 0) {
                        // Upscaling successful - get the upscaled output buffer
                        const uint32_t* upscaledBuffer = upscalerGetOutputBuffer();
                        
                        if (upscaledBuffer != nullptr) {
                            // Upload upscaled output to texture
                            // The upscaled buffer is in ARGB8888 format and already at display resolution
                            int upscaledWidth = 0, upscaledHeight = 0;
                            upscalerGetOutputDimensions(upscaledWidth, upscaledHeight);
                            
                            // DEBUG: Log upscaler output (ALWAYS, even after first frame)
                            static int logCount = 0;
                            if (logCount < 3) {
                                logCount++;
                                FILE* pipelineLog = fopen("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 2\\pipeline.log", "a");
                                if (pipelineLog) {
                                    fprintf(pipelineLog, "\n[FRAME %d] UPSCALER OUTPUT\n", logCount);
                                    fprintf(pipelineLog, "  upscaledWidth=%d, upscaledHeight=%d\n", upscaledWidth, upscaledHeight);
                                    fprintf(pipelineLog, "  upscaledBuffer=%p\n", upscaledBuffer);
                                    fclose(pipelineLog);
                                }
                            }
                            
                            // Update the SDL texture with the upscaled buffer
                            if (gSdlTexture != nullptr && upscaledWidth > 0 && upscaledHeight > 0) {
                                // DEBUG: Log dimensions BEFORE storing them (execute immediately)
                                static bool firstFrameLogged = false;
                                if (!firstFrameLogged) {
                                    firstFrameLogged = true;
                                    FILE* debugLog = fopen("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 2\\upscale.log", "a");
                                    if (debugLog) {
                                        int texW, texH;
                                        SDL_QueryTexture(gSdlTexture, nullptr, nullptr, &texW, &texH);
                                        fprintf(debugLog, "[RENDER] CRITICAL DIMENSIONS:\n");
                                        fprintf(debugLog, "[RENDER]   Upscaler output: %dx%d\n", upscaledWidth, upscaledHeight);
                                        fprintf(debugLog, "[RENDER]   Texture size: %dx%d\n", texW, texH);
                                        fflush(debugLog);
                                        fclose(debugLog);
                                    }
                                }
                                
                                // Upload the upscaled content to the texture
                                // Use UpdateTexture with a target rect to place content at top-left
                                SDL_Rect uploadRect;
                                uploadRect.x = 0;
                                uploadRect.y = 0;
                                uploadRect.w = upscaledWidth;
                                uploadRect.h = upscaledHeight;
                                SDL_UpdateTexture(gSdlTexture, &uploadRect, upscaledBuffer, upscaledWidth * 4);
                                
                                upscalerDidRender = true;  // Mark that we handled rendering
                                actualUpscaledWidth = upscaledWidth;  // Store for srcRect adjustment
                                actualUpscaledHeight = upscaledHeight;
                            }
                        }
                    }
                }
            }
        }
    }

    Rect presenterBounds = getPresenterSurfaceBounds();
    const int presenterWidth = rectGetWidth(&presenterBounds);
    const int presenterHeight = rectGetHeight(&presenterBounds);
    SDL_Rect srcRect;
    srcRect.x = 0;
    srcRect.y = 0;
    
    // When upscaler renders, use full texture (it contains letterboxed content)
    if (upscalerDidRender) {
        int texW, texH;
        SDL_QueryTexture(gSdlTexture, nullptr, nullptr, &texW, &texH);
        srcRect.w = texW;
        srcRect.h = texH;
    } else {
        srcRect.w = presenterWidth;
        srcRect.h = presenterHeight;
    }

    // Phase 8.4: Dirty region tracking - only upload changed pixels
    // Phase 8.5: Use streaming texture upload for direct GPU memory access
    // CRITICAL: SKIP ALL TEXTURE UPLOAD if upscaler already handled it
    // The upscaler processes gSdlSurface (640x480 phantom display) with filters + scaling
    // and uploads the result directly to gSdlTexture. We must NOT override it!
    Rect dirtyRect;
    bool hasDirtyRegion = windowVirtualScreenGetDirtyRect(&dirtyRect);
    
    bool textureUploadOk = upscalerDidRender;  // If upscaler rendered, we're done
    bool usedStreamingUpload = false;
    Rect uploadRect = presenterBounds;
    
    // ONLY upload original surface if upscaler did NOT handle the frame
    if (!upscalerDidRender) {
      if (hasDirtyRegion) {
        // Clip dirty rect to presenter bounds
        if (dirtyRect.left < 0) dirtyRect.left = 0;
        if (dirtyRect.top < 0) dirtyRect.top = 0;
        if (dirtyRect.right >= presenterWidth) dirtyRect.right = presenterWidth - 1;
        if (dirtyRect.bottom >= presenterHeight) dirtyRect.bottom = presenterHeight - 1;
        
        const int dirtyWidth = rectGetWidth(&dirtyRect);
        const int dirtyHeight = rectGetHeight(&dirtyRect);
        
        if (dirtyWidth > 0 && dirtyHeight > 0) {
            uploadRect = dirtyRect;
            
            // Phase 8.5: Use streaming upload (SDL_LockTexture) as primary path
            // This provides direct GPU memory access without an intermediate copy
            if (settings.system.streaming_textures) {
                textureUploadOk = streamingSurfaceRectToTexture(gSdlTexture, gSdlTextureSurface, uploadRect);
                usedStreamingUpload = textureUploadOk;
            }
            
            // Fallback to SDL_UpdateTexture if streaming failed or disabled
            if (!textureUploadOk) {
                const int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
                const unsigned char* srcPixels = static_cast<const unsigned char*>(gSdlTextureSurface->pixels);
                const unsigned char* dirtyPixels = srcPixels + dirtyRect.top * gSdlTextureSurface->pitch + dirtyRect.left * bytesPerPixel;
                
                SDL_Rect sdlDirtyRect;
                sdlDirtyRect.x = dirtyRect.left;
                sdlDirtyRect.y = dirtyRect.top;
                sdlDirtyRect.w = dirtyWidth;
                sdlDirtyRect.h = dirtyHeight;
                
                textureUploadOk = SDL_UpdateTexture(gSdlTexture, &sdlDirtyRect, dirtyPixels, gSdlTextureSurface->pitch) == 0;
            }
            
            if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
                const int fullPixels = presenterWidth * presenterHeight;
                const int dirtyPixelCount = dirtyWidth * dirtyHeight;
                const int savingsPercent = fullPixels > 0 ? 100 - (dirtyPixelCount * 100 / fullPixels) : 0;
                diagnosticsLog(DiagnosticsLevel::Trace,
                    "TEXTURE_UPLOAD",
                    "partial %s (%d,%d %dx%d) saved %d%%",
                    usedStreamingUpload ? "streaming" : "update",
                    dirtyRect.left, dirtyRect.top, dirtyWidth, dirtyHeight, savingsPercent);
            }
        } else {
            // Empty dirty region, nothing to upload
            textureUploadOk = true;
        }
      } else {
        // No dirty region tracked - upload full frame from original surface
        // Phase 8.5: Use streaming upload for full frame too
        if (settings.system.streaming_textures) {
            textureUploadOk = streamingSurfaceRectToTexture(gSdlTexture, gSdlTextureSurface, presenterBounds);
            usedStreamingUpload = textureUploadOk;
        }
        
        if (!textureUploadOk) {
            textureUploadOk = SDL_UpdateTexture(gSdlTexture, nullptr, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch) == 0;
        }
      }
    } // End of !upscalerDidRender block
    
    if (!textureUploadOk) {
        if (gTextureUploadFailureLogBudget > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "texture upload failed: %s", SDL_GetError());
            gTextureUploadFailureLogBudget--;
            if (gTextureUploadFailureLogBudget == 0) {
                diagnosticsLog(DiagnosticsLevel::Info, "RENDERER", "texture upload failure logging budget exhausted");
            }
        }
    }

    const char* textureUploadLabel = usedStreamingUpload ? "after_streaming_upload" : "after_texture_upload";
    if (!textureUploadOk) {
        textureUploadLabel = "after_texture_upload_failed";
    }
    logTextureUploadRectStats(uploadRect, textureUploadLabel);
    const Rect& viewport = displayScalerGetPhysicalViewport();
    logViewportIfChanged(viewport);
    SDL_Rect destRect;
    
    // When upscaler renders, display full texture at 1:1 centered on display
    // The texture already contains letterboxed content (e.g., 1920x1440 in 2560x1440 with black bars)
    if (upscalerDidRender) {
        const int physicalWidth = screenGetPhysicalWidth();
        const int physicalHeight = screenGetPhysicalHeight();
        int texW, texH;
        SDL_QueryTexture(gSdlTexture, nullptr, nullptr, &texW, &texH);
        
        // Render full texture at 1:1, centered
        destRect.w = texW;
        destRect.h = texH;
        destRect.x = (physicalWidth - texW) / 2;
        destRect.y = (physicalHeight - texH) / 2;
    } else {
        // Normal rendering path - use display scaler viewport
        destRect.x = viewport.left;
        destRect.y = viewport.top;
        destRect.w = rectGetWidth(&viewport);
        destRect.h = rectGetHeight(&viewport);
    }

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
    // Note: Overlay texture is at physical resolution (1920x1440), so use nullptr for src
    // to copy the full texture directly to the viewport destination
    if (gGpuOverlayEnabled && gGpuOverlayHasContent && gSdlOverlayTexture != nullptr) {
        SDL_RenderCopy(gSdlRenderer, gSdlOverlayTexture, nullptr, &destRect);
        
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
