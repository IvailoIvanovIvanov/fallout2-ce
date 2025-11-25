#include "window_manager.h"

#include <string.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>

#include <SDL.h>

#include "display_scaler.h"
#include "diagnostics.h"
#include "color.h"
#include "debug.h"
#include "dinput.h"
#include "draw.h"
#include "geometry.h"
#include "input.h"
#include "memory.h"
#include "mouse.h"
#include "palette.h"
#include "render_trace.h"
#include "svga.h"
#include "text_font.h"
#include "settings.h"
#include "win32.h"
#include "window_manager_private.h"

namespace fallout {

#define MAX_WINDOW_COUNT (50)

// The maximum number of button groups.
#define BUTTON_GROUP_LIST_CAPACITY (64)

static void windowFree(int win);
static void _win_buffering(bool a1);
static void _win_move(int win_index, int x, int y);
static void _win_clip(Window* window, RectListNode** rect, unsigned char* a3);
static void win_drag(int win);
static void _refresh_all(Rect* rect, unsigned char* a2);
static Button* buttonGetButton(int btn, Window** out_win);
static Button* buttonCreateInternal(int win, int x, int y, int width, int height, int mouseEnterEventCode, int mouseExitEventCode, int mouseDownEventCode, int mouseUpEventCode, int flags, unsigned char* up, unsigned char* dn, unsigned char* hover);
static int _GNW_check_buttons(Window* window, int* keyCodePtr);
static bool _button_under_mouse(Button* button, Rect* rect);
static void buttonFree(Button* ptr);
static int button_new_id();
static int _win_group_check_buttons(int buttonCount, int* btns, int maxChecked, RadioButtonCallback* func);
static int _button_check_group(Button* button);
static void _button_draw(Button* button, Window* window, unsigned char* data, bool draw, Rect* bound, bool sound);
static void _GNW_button_refresh(Window* window, Rect* rect);
static void virtualScreenResetDirty();
static void virtualScreenInvalidateRect(const Rect* rect);
static void virtualScreenInvalidateAll();
static void logVirtualScreenRectStats(const Rect& rect);
static int windowCompositeTrueColorOverlays(const Rect& rect);
static int windowScrubTrueColorMask(uint32_t* overlayStart, unsigned char* maskStart, int pitch, int width, int height);
static bool shouldAllocatePhysicalTrueColorOverlay();
static bool rectEquals(const Rect& a, const Rect& b);
static void windowClearPhysicalTrueColorOverlay(Window* window);
static void windowFreePhysicalTrueColorOverlay(Window* window);
static bool windowEnsurePhysicalTrueColorOverlay(Window* window, const Rect& viewport, bool viewportChanged);
static void windowSyncPhysicalTrueColorBuffersIfNeeded();
static void windowResetPresentStats();
static void windowAccumulateLogicalRectStats(const Rect& rect);
static void windowAccumulatePhysicalRectStats(const Rect& rect);
static void windowAccumulateHdLogicalPixels(int count);
static void windowAccumulateHdPhysicalPixels(int count);
static void windowLogPresentStats(const Rect& logicalRect, const Rect& physicalRect);

// 0x50FA30
static char _path_patches[] = "";

// 0x51E3D8
static bool _GNW95_already_running = false;

#ifdef _WIN32
// 0x51E3DC
static HANDLE _GNW95_title_mutex = INVALID_HANDLE_VALUE;
#endif

// 0x51E3E0
bool gWindowSystemInitialized = false;

// 0x51E3E4
int _GNW_wcolor[6] = {
    0,
    0,
    0,
    0,
    0,
    0,
};

// 0x51E3FC
static unsigned char* _screen_buffer = nullptr;
static int _screen_buffer_pitch = 0;
static bool gVirtualScreenEnabled = false;
static Rect gVirtualScreenDirtyRect = { 0, 0, -1, -1 };
static bool gVirtualScreenDirty = false;

// 0x51E400
static bool _insideWinExit = false;

// 0x51E404
static int _last_button_winID = -1;

// 0x6ADD90
static int gWindowIndexes[MAX_WINDOW_COUNT];

// 0x6ADE58
static Window* gWindows[MAX_WINDOW_COUNT];

// 0x6ADF20
static VideoSystemExitProc* gVideoSystemExitProc;

// 0x6ADF24
static int gWindowsLength;

// 0x6ADF28
static int _window_flags;

// 0x6ADF2C
static bool _buffering;

// 0x6ADF30
static int _bk_color;

// 0x6ADF34
static VideoSystemInitProc* gVideoSystemInitProc;

// 0x6ADF38
static int _doing_refresh_all;

// 0x6ADF3C
static void* _GNW_texture;

// 0x6ADF40
static ButtonGroup gButtonGroups[BUTTON_GROUP_LIST_CAPACITY];
static const int kVirtualScreenTraceMinArea = 10000;
static Rect gPhysicalTrueColorViewport = { 0, 0, -1, -1 };
static bool gPhysicalTrueColorViewportValid = false;
static uint32_t gPhysicalTrueColorOverlayRevision = 1;
struct WindowPresentStats {
    uint64_t logicalPixels = 0;
    uint64_t physicalPixels = 0;
    uint64_t hdLogicalPixels = 0;
    uint64_t hdPhysicalPixels = 0;
};
static WindowPresentStats gWindowPresentStats;

static bool rectEquals(const Rect& a, const Rect& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static void windowResetPresentStats()
{
    gWindowPresentStats = {};
}

static void windowAccumulateLogicalRectStats(const Rect& rect)
{
    int width = rectGetWidth(&rect);
    int height = rectGetHeight(&rect);
    if (width > 0 && height > 0) {
        gWindowPresentStats.logicalPixels += static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    }
}

static void windowAccumulatePhysicalRectStats(const Rect& rect)
{
    int width = rectGetWidth(&rect);
    int height = rectGetHeight(&rect);
    if (width > 0 && height > 0) {
        gWindowPresentStats.physicalPixels += static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    }
}

static void windowAccumulateHdLogicalPixels(int count)
{
    if (count > 0) {
        gWindowPresentStats.hdLogicalPixels += static_cast<uint64_t>(count);
    }
}

static void windowAccumulateHdPhysicalPixels(int count)
{
    if (count > 0) {
        gWindowPresentStats.hdPhysicalPixels += static_cast<uint64_t>(count);
    }
}

static void windowLogPresentStats(const Rect& logicalRect, const Rect& physicalRect)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        return;
    }

    const double scale = displayScalerGetScale();
    diagnosticsLog(DiagnosticsLevel::Trace,
        "SCALER",
        "present_stats logical=(%d,%d %dx%d) physical=(%d,%d %dx%d) logical_px=%llu physical_px=%llu hd_physical=%llu hd_logical=%llu hd_total=%llu scale=%.4f",
        logicalRect.left,
        logicalRect.top,
        rectGetWidth(&logicalRect),
        rectGetHeight(&logicalRect),
        physicalRect.left,
        physicalRect.top,
        rectGetWidth(&physicalRect),
        rectGetHeight(&physicalRect),
        static_cast<unsigned long long>(gWindowPresentStats.logicalPixels),
        static_cast<unsigned long long>(gWindowPresentStats.physicalPixels),
        static_cast<unsigned long long>(gWindowPresentStats.hdPhysicalPixels),
        static_cast<unsigned long long>(gWindowPresentStats.hdLogicalPixels),
        static_cast<unsigned long long>(gWindowPresentStats.hdPhysicalPixels + gWindowPresentStats.hdLogicalPixels),
        scale);
}

static void logVirtualScreenRectStats(const Rect& rect)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        return;
    }

    if (_screen_buffer == nullptr || _screen_buffer_pitch == 0) {
        return;
    }

    int width = rectGetWidth(&rect);
    int height = rectGetHeight(&rect);
    if (width <= 0 || height <= 0) {
        return;
    }

    size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (totalPixels == 0) {
        return;
    }

    unsigned int minValue = 0xFF;
    unsigned int maxValue = 0;
    unsigned long long checksum = 0;

    size_t sampleFirstIndex = 0;
    size_t sampleMiddleIndex = totalPixels / 2;
    size_t sampleLastIndex = totalPixels - 1;
    unsigned int firstSample = 0;
    unsigned int middleSample = 0;
    unsigned int lastSample = 0;

    size_t currentIndex = 0;
    for (int y = 0; y < height; y++) {
        const unsigned char* row = _screen_buffer + (rect.top + y) * _screen_buffer_pitch + rect.left;
        for (int x = 0; x < width; x++, currentIndex++) {
            unsigned int value = row[x];
            checksum += value;
            if (value < minValue) {
                minValue = value;
            }
            if (value > maxValue) {
                maxValue = value;
            }

            if (currentIndex == sampleFirstIndex) {
                firstSample = value;
            }
            if (currentIndex == sampleMiddleIndex) {
                middleSample = value;
            }
            if (currentIndex == sampleLastIndex) {
                lastSample = value;
            }
        }
    }

    diagnosticsLog(DiagnosticsLevel::Trace,
        "WINDOW",
        "virtualScreen stats rect=(%d,%d %dx%d) min=%u max=%u checksum=0x%llX samples=%u,%u,%u",
        rect.left,
        rect.top,
        width,
        height,
        minValue,
        maxValue,
        checksum,
        firstSample,
        middleSample,
        lastSample);
}

static void virtualScreenResetDirty()
{
    gVirtualScreenDirtyRect.left = 0;
    gVirtualScreenDirtyRect.top = 0;
    gVirtualScreenDirtyRect.right = -1;
    gVirtualScreenDirtyRect.bottom = -1;
    gVirtualScreenDirty = false;
}

static void virtualScreenInvalidateRect(const Rect* rect)
{
    if (!gVirtualScreenEnabled || rect == nullptr) {
        return;
    }

    Rect clipped;
    rectCopy(&clipped, rect);
    const Rect& bounds = displayScalerGetLogicalBounds();
    if (rectIntersection(&clipped, &bounds, &clipped) == -1) {
        return;
    }

    if (!gVirtualScreenDirty) {
        gVirtualScreenDirtyRect = clipped;
        gVirtualScreenDirty = true;
        return;
    }

    if (clipped.left < gVirtualScreenDirtyRect.left) {
        gVirtualScreenDirtyRect.left = clipped.left;
    }
    if (clipped.top < gVirtualScreenDirtyRect.top) {
        gVirtualScreenDirtyRect.top = clipped.top;
    }
    if (clipped.right > gVirtualScreenDirtyRect.right) {
        gVirtualScreenDirtyRect.right = clipped.right;
    }
    if (clipped.bottom > gVirtualScreenDirtyRect.bottom) {
        gVirtualScreenDirtyRect.bottom = clipped.bottom;
    }
}

static void virtualScreenInvalidateAll()
{
    if (!gVirtualScreenEnabled) {
        return;
    }

    Rect bounds = displayScalerGetLogicalBounds();
    virtualScreenInvalidateRect(&bounds);
}

static int windowScrubTrueColorMask(uint32_t* overlayStart, unsigned char* maskStart, int pitch, int width, int height)
{
    int sanitized = 0;

    for (int row = 0; row < height; row++) {
        uint32_t* overlayRow = overlayStart + row * pitch;
        unsigned char* maskRow = maskStart + row * pitch;

        for (int column = 0; column < width; column++) {
            if (maskRow[column] == 0) {
                if (overlayRow[column] != 0) {
                    overlayRow[column] = 0;
                }
                continue;
            }

            if (maskRow[column] != 1) {
                maskRow[column] = 1;
            }

            if ((overlayRow[column] >> 24) == 0) {
                maskRow[column] = 0;
                overlayRow[column] = 0;
                sanitized++;
            }
        }
    }

    return sanitized;
}

static bool shouldAllocatePhysicalTrueColorOverlay()
{
    if (!gVirtualScreenEnabled) {
        return false;
    }

    if (!settings.system.virtual_adapter_fullres) {
        return false;
    }

    constexpr double kScaleEpsilon = 1.0e-4;
    const double scale = displayScalerGetScale();
    return scale >= 1.0 - kScaleEpsilon;
}

static void windowClearPhysicalTrueColorOverlay(Window* window)
{
    if (window == nullptr) {
        return;
    }

    if (window->trueColorPhysicalOverlay == nullptr || window->trueColorPhysicalMask == nullptr) {
        return;
    }

    if (window->trueColorPhysicalWidth <= 0 || window->trueColorPhysicalHeight <= 0) {
        return;
    }

    size_t pixelCount = static_cast<size_t>(window->trueColorPhysicalWidth) * static_cast<size_t>(window->trueColorPhysicalHeight);
    memset(window->trueColorPhysicalOverlay, 0, pixelCount * sizeof(uint32_t));
    memset(window->trueColorPhysicalMask, 0, pixelCount);
}

static void windowFreePhysicalTrueColorOverlay(Window* window)
{
    if (window == nullptr) {
        return;
    }

    if (window->trueColorPhysicalOverlay != nullptr) {
        internal_free(window->trueColorPhysicalOverlay);
        window->trueColorPhysicalOverlay = nullptr;
    }

    if (window->trueColorPhysicalMask != nullptr) {
        internal_free(window->trueColorPhysicalMask);
        window->trueColorPhysicalMask = nullptr;
    }

    window->trueColorPhysicalPitch = 0;
    window->trueColorPhysicalWidth = 0;
    window->trueColorPhysicalHeight = 0;
    window->trueColorPhysicalViewport = { 0, 0, -1, -1 };
}

static bool windowEnsurePhysicalTrueColorOverlay(Window* window, const Rect& viewport, bool viewportChanged)
{
    if (window == nullptr) {
        return false;
    }

    const int viewportWidth = rectGetWidth(&viewport);
    const int viewportHeight = rectGetHeight(&viewport);

    if (viewportWidth <= 0 || viewportHeight <= 0) {
        if (window->trueColorPhysicalOverlay != nullptr || window->trueColorPhysicalMask != nullptr) {
            windowFreePhysicalTrueColorOverlay(window);
            return true;
        }
        return false;
    }

    bool changed = false;
    const bool hasBuffers = window->trueColorPhysicalOverlay != nullptr
        && window->trueColorPhysicalMask != nullptr
        && window->trueColorPhysicalPitch > 0;

    if (!hasBuffers || window->trueColorPhysicalWidth != viewportWidth || window->trueColorPhysicalHeight != viewportHeight) {
        windowFreePhysicalTrueColorOverlay(window);

        size_t pixelCount = static_cast<size_t>(viewportWidth) * static_cast<size_t>(viewportHeight);
        size_t overlayBytes = pixelCount * sizeof(uint32_t);
        uint32_t* overlay = (uint32_t*)internal_malloc(overlayBytes);
        unsigned char* mask = (unsigned char*)internal_malloc(pixelCount);

        if (overlay == nullptr || mask == nullptr) {
            if (overlay != nullptr) {
                internal_free(overlay);
            }
            if (mask != nullptr) {
                internal_free(mask);
            }

            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "SCALER",
                    "windowEnsurePhysicalTrueColorOverlay id=%d unable to allocate %zu byte physical true-color overlay",
                    window->id,
                    overlayBytes + pixelCount);
            }

            return true;
        }

        memset(overlay, 0, overlayBytes);
        memset(mask, 0, pixelCount);

        window->trueColorPhysicalOverlay = overlay;
        window->trueColorPhysicalMask = mask;
        window->trueColorPhysicalPitch = viewportWidth;
        window->trueColorPhysicalWidth = viewportWidth;
        window->trueColorPhysicalHeight = viewportHeight;
        window->trueColorPhysicalViewport = viewport;
        changed = true;
    } else if (!rectEquals(window->trueColorPhysicalViewport, viewport)) {
        window->trueColorPhysicalViewport = viewport;
        windowClearPhysicalTrueColorOverlay(window);
        changed = true;
    } else if (viewportChanged) {
        windowClearPhysicalTrueColorOverlay(window);
        changed = true;
    }

    return changed;
}

static void windowSyncPhysicalTrueColorBuffersIfNeeded()
{
    if (!gWindowSystemInitialized) {
        return;
    }

    const bool shouldHaveBuffers = shouldAllocatePhysicalTrueColorOverlay();
    const Rect& viewport = displayScalerGetPhysicalViewport();
    const int viewportWidth = rectGetWidth(&viewport);
    const int viewportHeight = rectGetHeight(&viewport);
    const bool viewportValid = viewportWidth > 0 && viewportHeight > 0;

    if (!shouldHaveBuffers || !viewportValid) {
        bool freed = false;
        for (int index = 0; index < gWindowsLength; index++) {
            Window* window = gWindows[index];
            if (window == nullptr) {
                continue;
            }

            if (window->trueColorPhysicalOverlay != nullptr || window->trueColorPhysicalMask != nullptr) {
                windowFreePhysicalTrueColorOverlay(window);
                freed = true;
            }
        }

        if (freed) {
            gPhysicalTrueColorOverlayRevision++;
        }

        gPhysicalTrueColorViewportValid = false;
        gPhysicalTrueColorViewport = { 0, 0, -1, -1 };
        return;
    }

    bool viewportChanged = !gPhysicalTrueColorViewportValid || !rectEquals(gPhysicalTrueColorViewport, viewport);
    bool updated = false;

    for (int index = 0; index < gWindowsLength; index++) {
        Window* window = gWindows[index];
        if (window == nullptr) {
            continue;
        }

        if (windowEnsurePhysicalTrueColorOverlay(window, viewport, viewportChanged)) {
            updated = true;
        }
    }

    if (updated) {
        gPhysicalTrueColorOverlayRevision++;
    }

    gPhysicalTrueColorViewportValid = true;
    gPhysicalTrueColorViewport = viewport;
}

static int windowCompositeTrueColorOverlays(const Rect& rect)
{
    if (!gVirtualScreenEnabled) {
        return 0;
    }

    windowRefreshPhysicalTrueColorBuffers();

    int pixelsOverridden = 0;

    for (int index = 0; index < gWindowsLength; index++) {
        Window* window = gWindows[index];
        if (window == nullptr) {
            continue;
        }

        const bool hasLogicalOverlay = window->trueColorOverlay != nullptr && window->trueColorMask != nullptr;
        const bool hasPhysicalOverlay = window->trueColorPhysicalOverlay != nullptr
            && window->trueColorPhysicalMask != nullptr
            && window->trueColorPhysicalPitch > 0
            && window->trueColorPhysicalWidth > 0
            && window->trueColorPhysicalHeight > 0;

        if (!hasLogicalOverlay && !hasPhysicalOverlay) {
            continue;
        }

        Rect clipped;
        if (rectIntersection(&(window->rect), &rect, &clipped) == -1) {
            continue;
        }

        int width = rectGetWidth(&clipped);
        int height = rectGetHeight(&clipped);
        if (width <= 0 || height <= 0) {
            continue;
        }

        bool attemptedPhysicalComposite = false;
        if (hasPhysicalOverlay) {
            Rect physicalRect = displayScalerLogicalToPhysical(clipped);
            Rect viewport = window->trueColorPhysicalViewport;
            if (rectIntersection(&physicalRect, &viewport, &physicalRect) != -1) {
                int physicalWidth = rectGetWidth(&physicalRect);
                int physicalHeight = rectGetHeight(&physicalRect);
                if (physicalWidth > 0 && physicalHeight > 0) {
                    attemptedPhysicalComposite = true;
                    int destLeft = physicalRect.left - viewport.left;
                    int destTop = physicalRect.top - viewport.top;
                    uint32_t* overlayStart = window->trueColorPhysicalOverlay + destTop * window->trueColorPhysicalPitch + destLeft;
                    unsigned char* maskStart = window->trueColorPhysicalMask + destTop * window->trueColorPhysicalPitch + destLeft;

                    int sanitized = windowScrubTrueColorMask(overlayStart, maskStart, window->trueColorPhysicalPitch, physicalWidth, physicalHeight);
                    if (sanitized > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                        diagnosticsLog(DiagnosticsLevel::Info,
                            "SCALER",
                            "windowCompositeTrueColorOverlays scrubbed=%d window=%d physical=(%d,%d %dx%d)",
                            sanitized,
                            window->id,
                            physicalRect.left,
                            physicalRect.top,
                            physicalWidth,
                            physicalHeight);
                    }

                    Rect presenterRect = physicalRect;
                    rectOffset(&presenterRect, -viewport.left, -viewport.top);
                    int written = blitPhysicalTrueColorRectToTexture(overlayStart, maskStart, window->trueColorPhysicalPitch, presenterRect);
                    windowAccumulateHdPhysicalPixels(written);
                    pixelsOverridden += written;
                }
            }
        }

        if (attemptedPhysicalComposite || !hasLogicalOverlay) {
            continue;
        }

        int offsetX = clipped.left - window->rect.left;
        int offsetY = clipped.top - window->rect.top;
        uint32_t* overlayStart = window->trueColorOverlay + offsetY * window->width + offsetX;
        unsigned char* maskStart = window->trueColorMask + offsetY * window->width + offsetX;

        int sanitized = windowScrubTrueColorMask(overlayStart, maskStart, window->width, width, height);
        if (sanitized > 0 && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "SCALER",
                "windowCompositeTrueColorOverlays scrubbed=%d window=%d logical=(%d,%d %dx%d)",
                sanitized,
                window->id,
                clipped.left,
                clipped.top,
                width,
                height);
        }

        int written = blitTrueColorRectToTexture(overlayStart, maskStart, window->width, clipped);
        windowAccumulateHdLogicalPixels(written);
        pixelsOverridden += written;
    }

    return pixelsOverridden;
}

// 0x4D5C30
int windowManagerInit(VideoSystemInitProc* videoSystemInitProc, VideoSystemExitProc* videoSystemExitProc, int a3)
{
#ifdef _WIN32
    CloseHandle(GNW95_mutex);
    GNW95_mutex = INVALID_HANDLE_VALUE;
#endif

    if (_GNW95_already_running) {
        return WINDOW_MANAGER_ERR_ALREADY_RUNNING;
    }

#ifdef _WIN32
    if (_GNW95_title_mutex == INVALID_HANDLE_VALUE) {
        return WINDOW_MANAGER_ERR_TITLE_NOT_SET;
    }
#endif

    if (gWindowSystemInitialized) {
        return WINDOW_MANAGER_ERR_WINDOW_SYSTEM_ALREADY_INITIALIZED;
    }

    for (int index = 0; index < MAX_WINDOW_COUNT; index++) {
        gWindowIndexes[index] = -1;
    }

    if (db_total() == 0) {
        if (dbOpen(nullptr, 0, _path_patches, 1) == -1) {
            return WINDOW_MANAGER_ERR_INITIALIZING_DEFAULT_DATABASE;
        }
    }

    if (textFontsInit() == -1) {
        return WINDOW_MANAGER_ERR_INITIALIZING_TEXT_FONTS;
    }

    _get_start_mode_();

    gVideoSystemInitProc = videoSystemInitProc;
    gVideoSystemExitProc = directInputFree;

    int rc = videoSystemInitProc();
    if (rc == -1) {
        if (gVideoSystemExitProc != nullptr) {
            gVideoSystemExitProc();
        }

        return WINDOW_MANAGER_ERR_INITIALIZING_VIDEO_MODE;
    }

    if (rc == 8) {
        return WINDOW_MANAGER_ERR_8;
    }

    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight();

    bool wantScreenBuffer = (a3 & 1) != 0;
    if (settings.system.virtual_adapter) {
        wantScreenBuffer = true;
    }

    if (wantScreenBuffer) {
        _screen_buffer = (unsigned char*)internal_malloc(screenWidth * screenHeight);
        if (_screen_buffer == nullptr) {
            if (gVideoSystemExitProc != nullptr) {
                gVideoSystemExitProc();
            } else {
                directDrawFree();
            }

            return WINDOW_MANAGER_ERR_NO_MEMORY;
        }

        _screen_buffer_pitch = screenWidth;
    } else {
        _screen_buffer = nullptr;
        _screen_buffer_pitch = 0;
    }

    gVirtualScreenEnabled = settings.system.virtual_adapter && _screen_buffer != nullptr;
    _buffering = gVirtualScreenEnabled;
    virtualScreenResetDirty();
    if (gVirtualScreenEnabled) {
        virtualScreenInvalidateAll();
    }
    _doing_refresh_all = 0;

    if (!_initColors()) {
        unsigned char* palette = (unsigned char*)internal_malloc(768);
        if (palette == nullptr) {
            if (gVideoSystemExitProc != nullptr) {
                gVideoSystemExitProc();
            } else {
                directDrawFree();
            }

            if (_screen_buffer != nullptr) {
                internal_free(_screen_buffer);
            }

            return WINDOW_MANAGER_ERR_NO_MEMORY;
        }

        bufferFill(palette, 768, 1, 768, 0);

        // TODO: Incomplete.
        // _colorBuildColorTable(_getSystemPalette(), palette);

        internal_free(palette);
    }

    _GNW_debug_init();

    if (inputInit(a3) == -1) {
        return WINDOW_MANAGER_ERR_INITIALIZING_INPUT;
    }

    _GNW_intr_init();

    Window* window = gWindows[0] = (Window*)internal_malloc(sizeof(*window));
    if (window == nullptr) {
        if (gVideoSystemExitProc != nullptr) {
            gVideoSystemExitProc();
        } else {
            directDrawFree();
        }

        if (_screen_buffer != nullptr) {
            internal_free(_screen_buffer);
        }

        return WINDOW_MANAGER_ERR_NO_MEMORY;
    }

    window->id = 0;
    window->flags = 0;
    window->rect.left = 0;
    window->rect.top = 0;
    window->rect.right = screenWidth - 1;
    window->rect.bottom = screenHeight - 1;
    window->width = screenWidth;
    window->height = screenHeight;
    window->tx = 0;
    window->ty = 0;
    window->buffer = nullptr;
    window->trueColorOverlay = nullptr;
    window->trueColorMask = nullptr;
    window->trueColorPhysicalOverlay = nullptr;
    window->trueColorPhysicalMask = nullptr;
    window->trueColorPhysicalPitch = 0;
    window->trueColorPhysicalWidth = 0;
    window->trueColorPhysicalHeight = 0;
    window->trueColorPhysicalViewport = { 0, 0, -1, -1 };
    window->trueColorPhysicalOverlay = nullptr;
    window->trueColorPhysicalMask = nullptr;
    window->trueColorPhysicalPitch = 0;
    window->trueColorPhysicalWidth = 0;
    window->trueColorPhysicalHeight = 0;
    window->trueColorPhysicalViewport = { 0, 0, -1, -1 };
    window->buttonListHead = nullptr;
    window->hoveredButton = nullptr;
    window->clickedButton = nullptr;
    window->menuBar = nullptr;

    gWindowsLength = 1;
    gWindowSystemInitialized = 1;
    _GNW_wcolor[3] = 21140;
    _GNW_wcolor[4] = 32747;
    _GNW_wcolor[5] = 31744;
    gWindowIndexes[0] = 0;
    _GNW_texture = nullptr;
    _bk_color = 0;
    _GNW_wcolor[0] = 10570;
    _window_flags = a3;
    _GNW_wcolor[2] = 8456;
    _GNW_wcolor[1] = 15855;

    atexit(windowManagerExit);

    return WINDOW_MANAGER_OK;
}

// 0x4D616C
void windowManagerExit(void)
{
    if (!_insideWinExit) {
        _insideWinExit = true;
        if (gWindowSystemInitialized) {
            _GNW_intr_exit();
            renderTraceReset();

            for (int index = gWindowsLength - 1; index >= 0; index--) {
                windowFree(gWindows[index]->id);
            }

            if (_GNW_texture != nullptr) {
                internal_free(_GNW_texture);
            }

            if (_screen_buffer != nullptr) {
                internal_free(_screen_buffer);
            }
            _screen_buffer = nullptr;
            _screen_buffer_pitch = 0;
            gVirtualScreenEnabled = false;
            virtualScreenResetDirty();

            if (gVideoSystemExitProc != nullptr) {
                gVideoSystemExitProc();
            }

            inputExit();
            _GNW_rect_exit();
            textFontsExit();
            _colorsClose();

            SDL_DestroyWindow(gSdlWindow);

            gWindowSystemInitialized = false;

#ifdef _WIN32
            CloseHandle(_GNW95_title_mutex);
            _GNW95_title_mutex = INVALID_HANDLE_VALUE;
#endif
        }
        _insideWinExit = false;
    }
}

// win_add
// 0x4D6238
int windowCreate(int x, int y, int width, int height, int color, int flags)
{
    int v23;
    int v25, v26;
    Window* tmp;

    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (gWindowsLength == MAX_WINDOW_COUNT) {
        return -1;
    }

    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight();

    if (width > screenWidth) {
        return -1;
    }

    if (height > screenHeight) {
        return -1;
    }

    Window* window = gWindows[gWindowsLength] = (Window*)internal_malloc(sizeof(*window));
    if (window == nullptr) {
        return -1;
    }

    window->buffer = (unsigned char*)internal_malloc(width * height);
    if (window->buffer == nullptr) {
        internal_free(window);
        return -1;
    }

    window->trueColorOverlay = nullptr;
    window->trueColorMask = nullptr;

    int id = 1;
    while (windowGetWindow(id) != nullptr) {
        id++;
    }

    window->id = id;

    if (gVirtualScreenEnabled) {
        size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        size_t overlayBytes = pixelCount * sizeof(uint32_t);
        uint32_t* overlay = nullptr;
        unsigned char* mask = nullptr;

        if (pixelCount > 0) {
            overlay = (uint32_t*)internal_malloc(overlayBytes);
            mask = (unsigned char*)internal_malloc(pixelCount);
        }

        if (overlay == nullptr || mask == nullptr) {
            if (overlay != nullptr) {
                internal_free(overlay);
            }
            if (mask != nullptr) {
                internal_free(mask);
            }

            if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                diagnosticsLog(DiagnosticsLevel::Info,
                    "SCALER",
                    "windowCreate id=%d unable to allocate %zu byte true-color overlay",
                    window->id,
                    overlayBytes + pixelCount);
            }
        } else {
            memset(overlay, 0, overlayBytes);
            memset(mask, 0, pixelCount);
            window->trueColorOverlay = overlay;
            window->trueColorMask = mask;
        }
    }

    if (shouldAllocatePhysicalTrueColorOverlay()) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        const int viewportWidth = rectGetWidth(&viewport);
        const int viewportHeight = rectGetHeight(&viewport);
        if (viewportWidth > 0 && viewportHeight > 0) {
            size_t pixelCount = static_cast<size_t>(viewportWidth) * static_cast<size_t>(viewportHeight);
            size_t overlayBytes = pixelCount * sizeof(uint32_t);
            uint32_t* overlay = (uint32_t*)internal_malloc(overlayBytes);
            unsigned char* mask = (unsigned char*)internal_malloc(pixelCount);

            if (overlay == nullptr || mask == nullptr) {
                if (overlay != nullptr) {
                    internal_free(overlay);
                }
                if (mask != nullptr) {
                    internal_free(mask);
                }

                if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
                    diagnosticsLog(DiagnosticsLevel::Info,
                        "SCALER",
                        "windowCreate id=%d unable to allocate %zu byte physical true-color overlay",
                        window->id,
                        overlayBytes + pixelCount);
                }
            } else {
                memset(overlay, 0, overlayBytes);
                memset(mask, 0, pixelCount);
                window->trueColorPhysicalOverlay = overlay;
                window->trueColorPhysicalMask = mask;
                window->trueColorPhysicalPitch = viewportWidth;
                window->trueColorPhysicalWidth = viewportWidth;
                window->trueColorPhysicalHeight = viewportHeight;
                window->trueColorPhysicalViewport = viewport;
            }
        }
    }

    if ((flags & WINDOW_USE_DEFAULTS) != 0) {
        flags |= _window_flags;
    }

    window->width = width;
    window->height = height;
    window->flags = flags;
    window->tx = rand() & 0xFFFE;
    window->ty = rand() & 0xFFFE;

    if (color == 256) {
        if (_GNW_texture == nullptr) {
            color = _colorTable[_GNW_wcolor[0]];
        }
    } else if ((color & 0xFF00) != 0) {
        int colorIndex = (color & 0xFF) - 1;
        color = (color & ~0xFFFF) | _colorTable[_GNW_wcolor[colorIndex]];
    }

    window->buttonListHead = nullptr;
    window->hoveredButton = nullptr;
    window->clickedButton = nullptr;
    window->menuBar = nullptr;
    window->blitProc = blitBufferToBufferTrans;
    window->color = color;
    gWindowIndexes[id] = gWindowsLength;
    gWindowsLength++;

    windowFill(id, 0, 0, width, height, color);

    window->flags |= WINDOW_HIDDEN;
    _win_move(id, x, y);
    window->flags = flags;

    if ((flags & WINDOW_MOVE_ON_TOP) == 0) {
        v23 = gWindowsLength - 2;
        while (v23 > 0) {
            if (!(gWindows[v23]->flags & WINDOW_MOVE_ON_TOP)) {
                break;
            }
            v23--;
        }

        if (v23 != gWindowsLength - 2) {
            v25 = v23 + 1;
            v26 = gWindowsLength - 1;
            while (v26 > v25) {
                tmp = gWindows[v26 - 1];
                gWindows[v26] = tmp;
                gWindowIndexes[tmp->id] = v26;
                v26--;
            }

            gWindows[v25] = window;
            gWindowIndexes[id] = v25;
        }
    }

    return id;
}

// win_remove
// 0x4D6468
void windowDestroy(int win)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    Rect rect;
    rectCopy(&rect, &(window->rect));

    int v1 = gWindowIndexes[window->id];
    windowFree(win);

    gWindowIndexes[win] = -1;

    for (int index = v1; index < gWindowsLength - 1; index++) {
        gWindows[index] = gWindows[index + 1];
        gWindowIndexes[gWindows[index]->id] = index;
    }

    gWindowsLength--;

    // NOTE: Uninline.
    windowRefreshAll(&rect);
}

// 0x4D650C
void windowFree(int win)
{
    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return;
    }

    if (window->buffer != nullptr) {
        internal_free(window->buffer);
    }

    if (window->trueColorOverlay != nullptr) {
        internal_free(window->trueColorOverlay);
    }

    if (window->trueColorMask != nullptr) {
        internal_free(window->trueColorMask);
    }

    windowFreePhysicalTrueColorOverlay(window);

    if (window->menuBar != nullptr) {
        internal_free(window->menuBar);
    }

    Button* curr = window->buttonListHead;
    while (curr != nullptr) {
        Button* next = curr->next;
        buttonFree(curr);
        curr = next;
    }

    internal_free(window);
}

// 0x4D6558
void _win_buffering(bool a1)
{
    if (_screen_buffer != nullptr) {
        if (gVirtualScreenEnabled && !a1) {
            return;
        }
        _buffering = a1;
    }
}

// 0x4D6568
void windowDrawBorder(int win)
{
    if (!gWindowSystemInitialized) {
        return;
    }

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return;
    }

    _lighten_buf(window->buffer + 5, window->width - 10, 5, window->width);
    _lighten_buf(window->buffer, 5, window->height, window->width);
    _lighten_buf(window->buffer + window->width - 5, 5, window->height, window->width);
    _lighten_buf(window->buffer + window->width * (window->height - 5) + 5, window->width - 10, 5, window->width);

    bufferDrawRect(window->buffer, window->width, 0, 0, window->width - 1, window->height - 1, _colorTable[0]);

    bufferDrawRectShadowed(window->buffer, window->width, 1, 1, window->width - 2, window->height - 2, _colorTable[_GNW_wcolor[1]], _colorTable[_GNW_wcolor[2]]);
    bufferDrawRectShadowed(window->buffer, window->width, 5, 5, window->width - 6, window->height - 6, _colorTable[_GNW_wcolor[2]], _colorTable[_GNW_wcolor[1]]);
}

// 0x4D684C
void windowDrawText(int win, const char* str, int width, int x, int y, int color)
{
    unsigned char* buf;
    int textColor;

    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    if (width == 0) {
        if (color & 0x040000) {
            width = fontGetMonospacedStringWidth(str);
        } else {
            width = fontGetStringWidth(str);
        }
    }

    if (width + x > window->width) {
        if (!(color & 0x04000000)) {
            return;
        }

        width = window->width - x;
    }

    buf = window->buffer + x + y * window->width;

    if (fontGetLineHeight() + y > window->height) {
        return;
    }

    if (!(color & 0x02000000)) {
        if (window->color == 256 && _GNW_texture != nullptr) {
            _buf_texture(buf, width, fontGetLineHeight(), window->width, _GNW_texture, window->tx + x, window->ty + y);
        } else {
            bufferFill(buf, width, fontGetLineHeight(), window->width, window->color);
        }
    }

    if ((color & 0xFF00) != 0) {
        int colorIndex = (color & 0xFF) - 1;
        textColor = (color & ~0xFFFF) | _colorTable[_GNW_wcolor[colorIndex]];
    } else {
        textColor = color;
    }

    fontDrawText(buf, str, width, window->width, textColor);

    if (color & 0x01000000) {
        // TODO: Check.
        Rect rect;
        rect.left = window->rect.left + x;
        rect.top = window->rect.top + y;
        rect.right = rect.left + width;
        rect.bottom = rect.top + fontGetLineHeight();
        _GNW_win_refresh(window, &rect, nullptr);
    }
}

// 0x4D6B24
void windowDrawLine(int win, int left, int top, int right, int bottom, int color)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    if ((color & 0xFF00) != 0) {
        int colorIndex = (color & 0xFF) - 1;
        color = (color & ~0xFFFF) | _colorTable[_GNW_wcolor[colorIndex]];
    }

    bufferDrawLine(window->buffer, window->width, left, top, right, bottom, color);
}

// 0x4D6B88
void windowDrawRect(int win, int left, int top, int right, int bottom, int color)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    if ((color & 0xFF00) != 0) {
        int colorIndex = (color & 0xFF) - 1;
        color = (color & ~0xFFFF) | _colorTable[_GNW_wcolor[colorIndex]];
    }

    if (right < left) {
        int tmp = left;
        left = right;
        right = tmp;
    }

    if (bottom < top) {
        int tmp = top;
        top = bottom;
        bottom = tmp;
    }

    bufferDrawRect(window->buffer, window->width, left, top, right, bottom, color);
}

// 0x4D6CC8
void windowFill(int win, int x, int y, int width, int height, int color)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    if (color == 256) {
        if (_GNW_texture != nullptr) {
            _buf_texture(window->buffer + window->width * y + x, width, height, window->width, _GNW_texture, x + window->tx, y + window->ty);
        } else {
            color = _colorTable[_GNW_wcolor[0]] & 0xFF;
        }
    } else if ((color & 0xFF00) != 0) {
        int colorIndex = (color & 0xFF) - 1;
        color = (color & ~0xFFFF) | _colorTable[_GNW_wcolor[colorIndex]];
    }

    if (color < 256) {
        bufferFill(window->buffer + window->width * y + x, width, height, window->width, color);
    }
}

// 0x4D6DAC
void windowShow(int win)
{
    Window* window = windowGetWindow(win);
    int index = gWindowIndexes[window->id];

    if (!gWindowSystemInitialized) {
        return;
    }

    if ((window->flags & WINDOW_HIDDEN) != 0) {
        window->flags &= ~WINDOW_HIDDEN;
        if (index == gWindowsLength - 1) {
            _GNW_win_refresh(window, &(window->rect), nullptr);
        }
    }

    if (index < gWindowsLength - 1 && (window->flags & WINDOW_DONT_MOVE_TOP) == 0) {
        while (index < gWindowsLength - 1) {
            Window* nextWindow = gWindows[index + 1];
            if ((window->flags & WINDOW_MOVE_ON_TOP) == 0 && (nextWindow->flags & WINDOW_MOVE_ON_TOP) != 0) {
                break;
            }

            gWindows[index] = nextWindow;
            gWindowIndexes[nextWindow->id] = index;
            index++;
        }

        gWindows[index] = window;
        gWindowIndexes[window->id] = index;
        _GNW_win_refresh(window, &(window->rect), nullptr);
    } else {
        // SFALL: Fix for the window with the "DontMoveTop" flag not being
        // redrawn after the show function call if it is not the topmost
        // one.
        _GNW_win_refresh(window, &(window->rect), nullptr);
    }
}

// 0x4D6E64
void windowHide(int win)
{
    if (!gWindowSystemInitialized) {
        return;
    }

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return;
    }

    if ((window->flags & WINDOW_HIDDEN) == 0) {
        window->flags |= WINDOW_HIDDEN;
        _refresh_all(&(window->rect), nullptr);
    }
}

// 0x4D6EA0
void _win_move(int win, int x, int y)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    Rect rect;
    rectCopy(&rect, &(window->rect));

    if (x < 0) {
        x = 0;
    }

    if (y < 0) {
        y = 0;
    }

    if ((window->flags & WINDOW_MANAGED) != 0) {
        x += 2;
    }

    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight();

    if (x + window->width - 1 > screenWidth - 1) {
        x = screenWidth - window->width;
    }

    if (y + window->height - 1 > screenHeight - 1) {
        y = screenHeight - window->height;
    }

    if ((window->flags & WINDOW_MANAGED) != 0) {
        // TODO: Not sure what this means.
        x &= ~0x03;
    }

    window->rect.left = x;
    window->rect.top = y;
    window->rect.right = window->width + x - 1;
    window->rect.bottom = window->height + y - 1;

    if ((window->flags & WINDOW_HIDDEN) == 0) {
        _GNW_win_refresh(window, &(window->rect), nullptr);

        if (gWindowSystemInitialized) {
            _refresh_all(&rect, nullptr);
        }
    }
}

// 0x4D6F5C
void windowRefresh(int win)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    _GNW_win_refresh(window, &(window->rect), nullptr);

    windowPresentVirtualScreen();
}

// 0x4D6F80
void windowRefreshRect(int win, const Rect* rect)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    Rect newRect;
    rectCopy(&newRect, rect);
    rectOffset(&newRect, window->rect.left, window->rect.top);

    _GNW_win_refresh(window, &newRect, nullptr);
    windowPresentVirtualScreen();
}

// 0x4D6FD8
void _GNW_win_refresh(Window* window, Rect* rect, unsigned char* a3)
{
    RectListNode *v26, *v20, *v23, *v24;
    int dest_pitch;

    // TODO: Get rid of this.
    dest_pitch = 0;

    const int screenWidth = screenGetWidth();

    if ((window->flags & WINDOW_HIDDEN) != 0) {
        return;
    }

    if ((window->flags & WINDOW_TRANSPARENT) && _buffering && !_doing_refresh_all) {
        // TODO: Incomplete.
    } else {
        v26 = _rect_malloc();
        if (v26 == nullptr) {
            return;
        }

        v26->next = nullptr;

        v26->rect.left = std::max(window->rect.left, rect->left);
        v26->rect.top = std::max(window->rect.top, rect->top);
        v26->rect.right = std::min(window->rect.right, rect->right);
        v26->rect.bottom = std::min(window->rect.bottom, rect->bottom);

        if (v26->rect.right >= v26->rect.left && v26->rect.bottom >= v26->rect.top) {
            if (a3) {
                dest_pitch = rect->right - rect->left + 1;
            }

            _win_clip(window, &v26, a3);

            if (window->id) {
                v20 = v26;
                while (v20) {
                    _GNW_button_refresh(window, &(v20->rect));

                    if (a3) {
                        if (_buffering && (window->flags & WINDOW_TRANSPARENT)) {
                            window->blitProc(window->buffer + v20->rect.left - window->rect.left + (v20->rect.top - window->rect.top) * window->width,
                                v20->rect.right - v20->rect.left + 1,
                                v20->rect.bottom - v20->rect.top + 1,
                                window->width,
                                a3 + dest_pitch * (v20->rect.top - rect->top) + v20->rect.left - rect->left,
                                dest_pitch);
                        } else {
                            blitBufferToBuffer(
                                window->buffer + v20->rect.left - window->rect.left + (v20->rect.top - window->rect.top) * window->width,
                                v20->rect.right - v20->rect.left + 1,
                                v20->rect.bottom - v20->rect.top + 1,
                                window->width,
                                a3 + dest_pitch * (v20->rect.top - rect->top) + v20->rect.left - rect->left,
                                dest_pitch);
                        }
                    } else {
                        if (_buffering) {
                            if (window->flags & WINDOW_TRANSPARENT) {
                                window->blitProc(
                                    window->buffer + v20->rect.left - window->rect.left + (v20->rect.top - window->rect.top) * window->width,
                                    v20->rect.right - v20->rect.left + 1,
                                    v20->rect.bottom - v20->rect.top + 1,
                                    window->width,
                                    _screen_buffer + v20->rect.top * screenWidth + v20->rect.left,
                                    screenWidth);
                            } else {
                                blitBufferToBuffer(
                                    window->buffer + v20->rect.left - window->rect.left + (v20->rect.top - window->rect.top) * window->width,
                                    v20->rect.right - v20->rect.left + 1,
                                    v20->rect.bottom - v20->rect.top + 1,
                                    window->width,
                                    _screen_buffer + v20->rect.top * screenWidth + v20->rect.left,
                                    screenWidth);
                            }
                        } else {
                            _scr_blit(
                                window->buffer + v20->rect.left - window->rect.left + (v20->rect.top - window->rect.top) * window->width,
                                window->width,
                                v20->rect.bottom - v20->rect.top + 1,
                                0,
                                0,
                                v20->rect.right - v20->rect.left + 1,
                                v20->rect.bottom - v20->rect.top + 1,
                                v20->rect.left,
                                v20->rect.top);
                        }
                    }

                    v20 = v20->next;
                }
            } else {
                RectListNode* v16 = v26;
                while (v16 != nullptr) {
                    int width = v16->rect.right - v16->rect.left + 1;
                    int height = v16->rect.bottom - v16->rect.top + 1;
                    unsigned char* buf = (unsigned char*)internal_malloc(width * height);
                    if (buf != nullptr) {
                        bufferFill(buf, width, height, width, _bk_color);
                        if (dest_pitch != 0) {
                            blitBufferToBuffer(
                                buf,
                                width,
                                height,
                                width,
                                a3 + dest_pitch * (v16->rect.top - rect->top) + v16->rect.left - rect->left,
                                dest_pitch);
                        } else {
                            if (_buffering) {
                                blitBufferToBuffer(buf,
                                    width,
                                    height,
                                    width,
                                    _screen_buffer + v16->rect.top * screenWidth + v16->rect.left,
                                    screenWidth);
                            } else {
                                _scr_blit(buf, width, height, 0, 0, width, height, v16->rect.left, v16->rect.top);
                            }
                        }

                        internal_free(buf);
                    }
                    v16 = v16->next;
                }
            }

            v23 = v26;
            while (v23) {
                v24 = v23->next;

                if (_buffering && !a3) {
                    if (gVirtualScreenEnabled) {
                        if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
                            const int width = rectGetWidth(&(v23->rect));
                            const int height = rectGetHeight(&(v23->rect));
                            const int area = width * height;
                            if (area >= kVirtualScreenTraceMinArea) {
                                diagnosticsLog(DiagnosticsLevel::Trace,
                                    "WINDOW",
                                    "virtualScreenInvalidate window=%d area=%d rect=(%d,%d %dx%d) buffering=%d refresh_all=%d",
                                    window->id,
                                    area,
                                    v23->rect.left,
                                    v23->rect.top,
                                    width,
                                    height,
                                    _buffering ? 1 : 0,
                                    _doing_refresh_all ? 1 : 0);
                            }
                        }
                        virtualScreenInvalidateRect(&(v23->rect));
                    } else {
                        _scr_blit(
                            _screen_buffer + v23->rect.left + screenWidth * v23->rect.top,
                            screenWidth,
                            v23->rect.bottom - v23->rect.top + 1,
                            0,
                            0,
                            v23->rect.right - v23->rect.left + 1,
                            v23->rect.bottom - v23->rect.top + 1,
                            v23->rect.left,
                            v23->rect.top);
                    }
                }

                _rect_free(v23);

                v23 = v24;
            }

            if (!_doing_refresh_all && a3 == nullptr && cursorIsHidden() == 0) {
                if (_mouse_in(rect->left, rect->top, rect->right, rect->bottom)) {
                    mouseShowCursor();
                }
            }
        } else {
            _rect_free(v26);
        }
    }
}

// 0x4D759C
void windowRefreshAll(Rect* rect)
{
    if (gWindowSystemInitialized) {
        _refresh_all(rect, nullptr);
    }
}

// 0x4D75B0
void _win_clip(Window* window, RectListNode** rectListNodePtr, unsigned char* a3)
{
    for (int index = gWindowIndexes[window->id] + 1; index < gWindowsLength; index++) {
        if (*rectListNodePtr == nullptr) {
            break;
        }

        Window* window = gWindows[index];
        if (!(window->flags & WINDOW_HIDDEN)) {
            if (!_buffering || !(window->flags & WINDOW_TRANSPARENT)) {
                _rect_clip_list(rectListNodePtr, &(window->rect));
            } else {
                if (!_doing_refresh_all) {
                    _GNW_win_refresh(window, &(window->rect), nullptr);
                    _rect_clip_list(rectListNodePtr, &(window->rect));
                }
            }
        }
    }

    if (a3 == _screen_buffer || a3 == nullptr) {
        if (cursorIsHidden() == 0) {
            Rect rect;
            mouseGetRect(&rect);
            _rect_clip_list(rectListNodePtr, &rect);
        }
    }
}

// 0x4D765C
void win_drag(int win)
{
    Window* window = windowGetWindow(win);
    int dx = 0;
    int dy = 0;
    int mx;
    int my;

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    windowShow(win);

    Rect rect = window->rect;

    tickersExecute();

    _mouse_info();

    while ((mouseGetEvent() & MOUSE_EVENT_ANY_BUTTON_UP) == 0) {
        sharedFpsLimiter.mark();

        if (dx != 0 || dy != 0) {
            window->rect.left += dx;
            window->rect.top += dy;
            window->rect.right += dx;
            window->rect.bottom += dy;
            _GNW_win_refresh(window, &(window->rect), nullptr);

            RectListNode* rectListNode = rect_clip(&rect, &(window->rect));
            while (rectListNode != nullptr) {
                RectListNode* next = rectListNode->next;

                // NOTE: Uninline.
                windowRefreshAll(&(rectListNode->rect));

                _rect_free(rectListNode);
                rectListNode = next;
            }

            rect = window->rect;
        }

        mouseGetPosition(&mx, &my);
        tickersExecute();

        _mouse_info();

        dx = mx;
        dy = my;
        mouseGetPosition(&mx, &my);

        dx = mx - dx;
        dy = my - dy;

        const Rect& logicalBounds = displayScalerGetLogicalBounds();

        if (dx + window->rect.left < logicalBounds.left) {
            dx = logicalBounds.left - window->rect.left;
        }

        if (dx + window->rect.right > logicalBounds.right) {
            dx = logicalBounds.right - window->rect.right;
        }

        if (dy + window->rect.top < logicalBounds.top) {
            dy = logicalBounds.top - window->rect.top;
        }

        if (dy + window->rect.bottom > logicalBounds.bottom) {
            dy = logicalBounds.bottom - window->rect.bottom;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if ((window->flags & WINDOW_MANAGED) != 0 && (window->rect.left & 3) != 0) {
        _win_move(window->id, window->rect.left, window->rect.top);
    }
}

// 0x4D77F8
void _win_get_mouse_buf(unsigned char* a1)
{
    Rect rect;
    mouseGetRect(&rect);
    _refresh_all(&rect, a1);
}

// 0x4D7814
void _refresh_all(Rect* rect, unsigned char* a2)
{
    _doing_refresh_all = 1;

    for (int index = 0; index < gWindowsLength; index++) {
        _GNW_win_refresh(gWindows[index], rect, a2);
    }

    _doing_refresh_all = 0;

    if (a2 == nullptr) {
        if (!cursorIsHidden()) {
            if (_mouse_in(rect->left, rect->top, rect->right, rect->bottom)) {
                mouseShowCursor();
            }
        }

        windowPresentVirtualScreen();
    }
}

// 0x4D7888
Window* windowGetWindow(int win)
{
    if (win == -1) {
        return nullptr;
    }

    int index = gWindowIndexes[win];
    if (index == -1) {
        return nullptr;
    }

    return gWindows[index];
}

// win_get_buf
// 0x4D78B0
unsigned char* windowGetBuffer(int win)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return nullptr;
    }

    if (window == nullptr) {
        return nullptr;
    }

    return window->buffer;
}

bool windowResolveBufferRect(const unsigned char* buffer, int pitch, int width, int height, Rect* outRect)
{
    if (!gWindowSystemInitialized) {
        return false;
    }

    if (buffer == nullptr || outRect == nullptr) {
        return false;
    }

    if (width <= 0 || height <= 0 || pitch <= 0) {
        return false;
    }

    auto tryResolve = [&](const unsigned char* base, int basePitch, int baseHeight, const Rect& baseRect) {
        if (base == nullptr) {
            return false;
        }

        if (pitch != basePitch) {
            return false;
        }

        ptrdiff_t delta = buffer - base;
        if (delta < 0) {
            return false;
        }

        const ptrdiff_t limit = static_cast<ptrdiff_t>(basePitch) * baseHeight;
        if (delta >= limit) {
            return false;
        }

        int localY = static_cast<int>(delta / basePitch);
        int localX = static_cast<int>(delta % basePitch);

        Rect resolved;
        resolved.left = baseRect.left + localX;
        resolved.top = baseRect.top + localY;
        resolved.right = resolved.left + width - 1;
        resolved.bottom = resolved.top + height - 1;

        if (rectIntersection(&resolved, &baseRect, &resolved) == -1) {
            return false;
        }

        *outRect = resolved;
        return true;
    };

    if (gVirtualScreenEnabled && _screen_buffer != nullptr) {
        const Rect& logical = displayScalerGetLogicalBounds();
        if (tryResolve(_screen_buffer, _screen_buffer_pitch, rectGetHeight(&logical), logical)) {
            return true;
        }
    }

    for (int index = 0; index < gWindowsLength; index++) {
        Window* window = gWindows[index];
        if (window == nullptr || window->buffer == nullptr) {
            continue;
        }

        Rect rect;
        rectCopy(&rect, &(window->rect));
        if (tryResolve(window->buffer, window->width, window->height, rect)) {
            return true;
        }
    }

    return false;
}

bool windowResolveTrueColorRegion(const unsigned char* buffer, int pitch, int width, int height, Rect* outRect, uint32_t** outOverlay, unsigned char** outMask, int* outOverlayPitch)
{
    if (!gWindowSystemInitialized) {
        return false;
    }

    if (buffer == nullptr || width <= 0 || height <= 0 || pitch <= 0) {
        return false;
    }

    for (int index = 0; index < gWindowsLength; index++) {
        Window* window = gWindows[index];
        if (window == nullptr || window->buffer == nullptr) {
            continue;
        }

        if (window->trueColorOverlay == nullptr || window->trueColorMask == nullptr) {
            continue;
        }

        if (pitch != window->width) {
            continue;
        }

        ptrdiff_t delta = buffer - window->buffer;
        if (delta < 0) {
            continue;
        }

        const ptrdiff_t limit = static_cast<ptrdiff_t>(window->width) * window->height;
        if (delta >= limit) {
            continue;
        }

        int localY = static_cast<int>(delta / window->width);
        int localX = static_cast<int>(delta % window->width);

        Rect resolved;
        resolved.left = window->rect.left + localX;
        resolved.top = window->rect.top + localY;
        resolved.right = resolved.left + width - 1;
        resolved.bottom = resolved.top + height - 1;

        Rect clipped;
        rectCopy(&clipped, &resolved);
        if (rectIntersection(&resolved, &(window->rect), &clipped) == -1) {
            continue;
        }

        if (clipped.left != resolved.left || clipped.top != resolved.top || clipped.right != resolved.right || clipped.bottom != resolved.bottom) {
            continue;
        }

        if (outRect != nullptr) {
            *outRect = clipped;
        }

        if (outOverlay != nullptr) {
            *outOverlay = window->trueColorOverlay + localY * window->width + localX;
        }

        if (outMask != nullptr) {
            *outMask = window->trueColorMask + localY * window->width + localX;
        }

        if (outOverlayPitch != nullptr) {
            *outOverlayPitch = window->width;
        }

        return true;
    }

    return false;
}

// 0x4D78CC
int windowGetAtPoint(int x, int y)
{
    for (int index = gWindowsLength - 1; index >= 0; index--) {
        Window* window = gWindows[index];
        if (x >= window->rect.left && x <= window->rect.right
            && y >= window->rect.top && y <= window->rect.bottom) {
            return window->id;
        }
    }

    return -1;
}

// 0x4D7918
int windowGetWidth(int win)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (window == nullptr) {
        return -1;
    }

    return window->width;
}

// 0x4D7934
int windowGetHeight(int win)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (window == nullptr) {
        return -1;
    }

    return window->height;
}

// win_get_rect
// 0x4D7950
int windowGetRect(int win, Rect* rect)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (window == nullptr) {
        return -1;
    }

    rectCopy(rect, &(window->rect));

    return 0;
}

// 0x4D797C
int _win_check_all_buttons()
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    int v1 = -1;
    for (int index = gWindowsLength - 1; index >= 1; index--) {
        if (_GNW_check_buttons(gWindows[index], &v1) == 0) {
            break;
        }

        if ((gWindows[index]->flags & WINDOW_MODAL) != 0) {
            break;
        }
    }

    return v1;
}

// 0x4D79DC
Button* buttonGetButton(int btn, Window** windowPtr)
{
    for (int index = 0; index < gWindowsLength; index++) {
        Window* window = gWindows[index];
        Button* button = window->buttonListHead;
        while (button != nullptr) {
            if (button->id == btn) {
                if (windowPtr != nullptr) {
                    *windowPtr = window;
                }

                return button;
            }
            button = button->next;
        }
    }

    return nullptr;
}

// 0x4D7A34
int _GNW_check_menu_bars(int input)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    for (int index = gWindowsLength - 1; index >= 1; index--) {
        Window* window = gWindows[index];
        if (window->menuBar != nullptr) {
            for (int pulldownIndex = 0; pulldownIndex < window->menuBar->pulldownsLength; pulldownIndex++) {
                if (input == window->menuBar->pulldowns[pulldownIndex].keyCode) {
                    input = _GNW_process_menu(window->menuBar, pulldownIndex);
                    break;
                }
            }
        }

        if ((window->flags & WINDOW_MODAL) != 0) {
            break;
        }
    }

    return input;
}

// 0x4D69DC
void _win_text(int win, char** fileNameList, int fileNameListLength, int maxWidth, int x, int y, int flags)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return;
    }

    if (window == nullptr) {
        return;
    }

    int width = window->width;
    unsigned char* ptr = window->buffer + y * width + x;
    int lineHeight = fontGetLineHeight();

    int step = width * lineHeight;
    int v1 = lineHeight / 2;
    int v2 = v1 + 1;
    int v3 = maxWidth - 1;

    for (int index = 0; index < fileNameListLength; index++) {
        char* fileName = fileNameList[index];
        if (*fileName != '\0') {
            windowDrawText(win, fileName, maxWidth, x, y, flags);
        } else {
            if (maxWidth != 0) {
                bufferDrawLine(ptr, width, 0, v1, v3, v1, _colorTable[_GNW_wcolor[2]]);
                bufferDrawLine(ptr, width, 0, v2, v3, v2, _colorTable[_GNW_wcolor[1]]);
            }
        }

        ptr += step;
        y += lineHeight;
    }
}

// 0x4D80D8
void programWindowSetTitle(const char* title)
{
    if (title == nullptr) {
        return;
    }

#ifdef _WIN32
    if (_GNW95_title_mutex == INVALID_HANDLE_VALUE) {
        _GNW95_title_mutex = CreateMutexA(nullptr, TRUE, title);
        if (GetLastError() != ERROR_SUCCESS) {
            _GNW95_already_running = true;
            return;
        }
    }
#endif

    strncpy(gProgramWindowTitle, title, 256);
    gProgramWindowTitle[256 - 1] = '\0';

    if (gSdlWindow != nullptr) {
        SDL_SetWindowTitle(gSdlWindow, gProgramWindowTitle);
    }
}

// 0x4D8200
bool showMesageBox(const char* text)
{
    SDL_Cursor* prev = SDL_GetCursor();
    SDL_Cursor* cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    SDL_SetCursor(cursor);
    SDL_ShowCursor(SDL_ENABLE);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, nullptr, text, nullptr);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_SetCursor(prev);
    SDL_FreeCursor(cursor);
    return true;
}

// 0x4D8260
int buttonCreate(int win, int x, int y, int width, int height, int mouseEnterEventCode, int mouseExitEventCode, int mouseDownEventCode, int mouseUpEventCode, unsigned char* up, unsigned char* dn, unsigned char* hover, int flags)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (window == nullptr) {
        return -1;
    }

    if (up == nullptr && (dn != nullptr || hover != nullptr)) {
        return -1;
    }

    Button* button = buttonCreateInternal(win, x, y, width, height, mouseEnterEventCode, mouseExitEventCode, mouseDownEventCode, mouseUpEventCode, flags | BUTTON_FLAG_GRAPHIC, up, dn, hover);
    if (button == nullptr) {
        return -1;
    }

    _button_draw(button, window, button->normalImage, false, nullptr, false);

    return button->id;
}

// 0x4D8308
int _win_register_text_button(int win, int x, int y, int mouseEnterEventCode, int mouseExitEventCode, int mouseDownEventCode, int mouseUpEventCode, const char* title, int flags)
{
    Window* window = windowGetWindow(win);

    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (window == nullptr) {
        return -1;
    }

    int buttonWidth = fontGetStringWidth(title) + 16;
    int buttonHeight = fontGetLineHeight() + 7;
    unsigned char* normal = (unsigned char*)internal_malloc(buttonWidth * buttonHeight);
    if (normal == nullptr) {
        return -1;
    }

    unsigned char* pressed = (unsigned char*)internal_malloc(buttonWidth * buttonHeight);
    if (pressed == nullptr) {
        internal_free(normal);
        return -1;
    }

    if (window->color == 256 && _GNW_texture != nullptr) {
        // TODO: Incomplete.
    } else {
        bufferFill(normal, buttonWidth, buttonHeight, buttonWidth, window->color);
        bufferFill(pressed, buttonWidth, buttonHeight, buttonWidth, window->color);
    }

    _lighten_buf(normal, buttonWidth, buttonHeight, buttonWidth);

    fontDrawText(normal + buttonWidth * 3 + 8, title, buttonWidth, buttonWidth, _colorTable[_GNW_wcolor[3]]);
    bufferDrawRectShadowed(normal,
        buttonWidth,
        2,
        2,
        buttonWidth - 3,
        buttonHeight - 3,
        _colorTable[_GNW_wcolor[1]],
        _colorTable[_GNW_wcolor[2]]);
    bufferDrawRectShadowed(normal,
        buttonWidth,
        1,
        1,
        buttonWidth - 2,
        buttonHeight - 2,
        _colorTable[_GNW_wcolor[1]],
        _colorTable[_GNW_wcolor[2]]);
    bufferDrawRect(normal, buttonWidth, 0, 0, buttonWidth - 1, buttonHeight - 1, _colorTable[0]);

    fontDrawText(pressed + buttonWidth * 4 + 9, title, buttonWidth, buttonWidth, _colorTable[_GNW_wcolor[3]]);
    bufferDrawRectShadowed(pressed,
        buttonWidth,
        2,
        2,
        buttonWidth - 3,
        buttonHeight - 3,
        _colorTable[_GNW_wcolor[2]],
        _colorTable[_GNW_wcolor[1]]);
    bufferDrawRectShadowed(pressed,
        buttonWidth,
        1,
        1,
        buttonWidth - 2,
        buttonHeight - 2,
        _colorTable[_GNW_wcolor[2]],
        _colorTable[_GNW_wcolor[1]]);
    bufferDrawRect(pressed, buttonWidth, 0, 0, buttonWidth - 1, buttonHeight - 1, _colorTable[0]);

    Button* button = buttonCreateInternal(win,
        x,
        y,
        buttonWidth,
        buttonHeight,
        mouseEnterEventCode,
        mouseExitEventCode,
        mouseDownEventCode,
        mouseUpEventCode,
        flags,
        normal,
        pressed,
        nullptr);
    if (button == nullptr) {
        internal_free(normal);
        internal_free(pressed);
        return -1;
    }

    _button_draw(button, window, button->normalImage, false, nullptr, false);

    return button->id;
}

// 0x4D8674
int _win_register_button_disable(int btn, unsigned char* up, unsigned char* down, unsigned char* hover)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Button* button = buttonGetButton(btn, nullptr);
    if (button == nullptr) {
        return -1;
    }

    button->disabledNormalImage = up;
    button->disabledPressedImage = down;
    button->disabledHoverImage = hover;

    return 0;
}

// 0x4D86A8
int _win_register_button_image(int btn, unsigned char* up, unsigned char* down, unsigned char* hover, bool draw)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (up == nullptr && (down != nullptr || hover != nullptr)) {
        return -1;
    }

    Window* window;
    Button* button = buttonGetButton(btn, &window);
    if (button == nullptr) {
        return -1;
    }

    if (!(button->flags & BUTTON_FLAG_GRAPHIC)) {
        return -1;
    }

    unsigned char* data = button->currentImage;
    if (data == button->normalImage) {
        button->currentImage = up;
    } else if (data == button->pressedImage) {
        button->currentImage = down;
    } else if (data == button->hoverImage) {
        button->currentImage = hover;
    }

    button->normalImage = up;
    button->pressedImage = down;
    button->hoverImage = hover;

    _button_draw(button, window, button->currentImage, draw, nullptr, false);

    return 0;
}

// Sets primitive callbacks on the button.
//
// 0x4D8758
int buttonSetMouseCallbacks(int btn, ButtonCallback* mouseEnterProc, ButtonCallback* mouseExitProc, ButtonCallback* mouseDownProc, ButtonCallback* mouseUpProc)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Button* button = buttonGetButton(btn, nullptr);
    if (button == nullptr) {
        return -1;
    }

    button->mouseEnterProc = mouseEnterProc;
    button->mouseExitProc = mouseExitProc;
    button->leftMouseDownProc = mouseDownProc;
    button->leftMouseUpProc = mouseUpProc;

    return 0;
}

// 0x4D8798
int buttonSetRightMouseCallbacks(int btn, int rightMouseDownEventCode, int rightMouseUpEventCode, ButtonCallback* rightMouseDownProc, ButtonCallback* rightMouseUpProc)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Button* button = buttonGetButton(btn, nullptr);
    if (button == nullptr) {
        return -1;
    }

    button->rightMouseDownEventCode = rightMouseDownEventCode;
    button->rightMouseUpEventCode = rightMouseUpEventCode;
    button->rightMouseDownProc = rightMouseDownProc;
    button->rightMouseUpProc = rightMouseUpProc;

    if (rightMouseDownEventCode != -1 || rightMouseUpEventCode != -1 || rightMouseDownProc != nullptr || rightMouseUpProc != nullptr) {
        button->flags |= BUTTON_FLAG_RIGHT_MOUSE_BUTTON_CONFIGURED;
    } else {
        button->flags &= ~BUTTON_FLAG_RIGHT_MOUSE_BUTTON_CONFIGURED;
    }

    return 0;
}

// Sets button state callbacks.
// [a2] - when button is transitioning to pressed state
// [a3] - when button is returned to unpressed state
//
// The changes in the state are tied to graphical state, therefore these callbacks are not generated for
// buttons with no graphics.
//
// These callbacks can be triggered several times during tracking if mouse leaves button's rectangle without releasing mouse buttons.
//
// 0x4D87F8
int buttonSetCallbacks(int btn, ButtonCallback* pressSoundFunc, ButtonCallback* releaseSoundFunc)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Button* button = buttonGetButton(btn, nullptr);
    if (button == nullptr) {
        return -1;
    }

    button->pressSoundFunc = pressSoundFunc;
    button->releaseSoundFunc = releaseSoundFunc;

    return 0;
}

// 0x4D8828
int buttonSetMask(int btn, unsigned char* mask)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Button* button = buttonGetButton(btn, nullptr);
    if (button == nullptr) {
        return -1;
    }

    button->mask = mask;

    return 0;
}

// 0x4D8854
Button* buttonCreateInternal(int win, int x, int y, int width, int height, int mouseEnterEventCode, int mouseExitEventCode, int mouseDownEventCode, int mouseUpEventCode, int flags, unsigned char* up, unsigned char* dn, unsigned char* hover)
{
    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return nullptr;
    }

    Button* button = (Button*)internal_malloc(sizeof(*button));
    if (button == nullptr) {
        return nullptr;
    }

    if ((flags & BUTTON_FLAG_0x01) == 0) {
        if ((flags & BUTTON_FLAG_0x02) != 0) {
            flags &= ~BUTTON_FLAG_0x02;
        }

        if ((flags & BUTTON_FLAG_0x04) != 0) {
            flags &= ~BUTTON_FLAG_0x04;
        }
    }

    // NOTE: Uninline.
    int buttonId = button_new_id();

    button->id = buttonId;
    button->flags = flags;
    button->rect.left = x;
    button->rect.top = y;
    button->rect.right = x + width - 1;
    button->rect.bottom = y + height - 1;
    button->mouseEnterEventCode = mouseEnterEventCode;
    button->mouseExitEventCode = mouseExitEventCode;
    button->lefMouseDownEventCode = mouseDownEventCode;
    button->leftMouseUpEventCode = mouseUpEventCode;
    button->rightMouseDownEventCode = -1;
    button->rightMouseUpEventCode = -1;
    button->normalImage = up;
    button->pressedImage = dn;
    button->hoverImage = hover;
    button->disabledNormalImage = nullptr;
    button->disabledPressedImage = nullptr;
    button->disabledHoverImage = nullptr;
    button->currentImage = nullptr;
    button->mask = nullptr;
    button->mouseEnterProc = nullptr;
    button->mouseExitProc = nullptr;
    button->leftMouseDownProc = nullptr;
    button->leftMouseUpProc = nullptr;
    button->rightMouseDownProc = nullptr;
    button->rightMouseUpProc = nullptr;
    button->pressSoundFunc = nullptr;
    button->releaseSoundFunc = nullptr;
    button->buttonGroup = nullptr;
    button->prev = nullptr;

    button->next = window->buttonListHead;
    if (button->next != nullptr) {
        button->next->prev = button;
    }
    window->buttonListHead = button;

    return button;
}

// 0x4D89E4
bool _win_button_down(int btn)
{
    if (!gWindowSystemInitialized) {
        return false;
    }

    Button* button = buttonGetButton(btn, nullptr);
    if (button == nullptr) {
        return false;
    }

    if ((button->flags & BUTTON_FLAG_0x01) != 0 && (button->flags & BUTTON_FLAG_CHECKED) != 0) {
        return true;
    }

    return false;
}

// 0x4D8A10
int _GNW_check_buttons(Window* window, int* keyCodePtr)
{
    Rect v58;
    Button* prevHoveredButton;
    Button* prevClickedButton;
    Button* button;

    if ((window->flags & WINDOW_HIDDEN) != 0) {
        return -1;
    }

    button = window->buttonListHead;
    prevHoveredButton = window->hoveredButton;
    prevClickedButton = window->clickedButton;

    if (prevHoveredButton != nullptr) {
        rectCopy(&v58, &(prevHoveredButton->rect));
        rectOffset(&v58, window->rect.left, window->rect.top);
    } else if (prevClickedButton != nullptr) {
        rectCopy(&v58, &(prevClickedButton->rect));
        rectOffset(&v58, window->rect.left, window->rect.top);
    }

    *keyCodePtr = -1;

    if (_mouse_click_in(window->rect.left, window->rect.top, window->rect.right, window->rect.bottom)) {
        int mouseEvent = mouseGetEvent();
        if ((window->flags & WINDOW_FLAG_0x40) || (mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) == 0) {
            if (mouseEvent == 0) {
                window->clickedButton = nullptr;
            }
        } else {
            windowShow(window->id);
        }

        if (prevHoveredButton != nullptr) {
            if (!_button_under_mouse(prevHoveredButton, &v58)) {
                if (!(prevHoveredButton->flags & BUTTON_FLAG_DISABLED)) {
                    *keyCodePtr = prevHoveredButton->mouseExitEventCode;
                }

                if ((prevHoveredButton->flags & BUTTON_FLAG_0x01) && (prevHoveredButton->flags & BUTTON_FLAG_CHECKED)) {
                    _button_draw(prevHoveredButton, window, prevHoveredButton->pressedImage, true, nullptr, true);
                } else {
                    _button_draw(prevHoveredButton, window, prevHoveredButton->normalImage, true, nullptr, true);
                }

                window->hoveredButton = nullptr;

                _last_button_winID = window->id;

                if (!(prevHoveredButton->flags & BUTTON_FLAG_DISABLED)) {
                    if (prevHoveredButton->mouseExitProc != nullptr) {
                        prevHoveredButton->mouseExitProc(prevHoveredButton->id, *keyCodePtr);
                        if (!(prevHoveredButton->flags & BUTTON_FLAG_0x40)) {
                            *keyCodePtr = -1;
                        }
                    }
                }
                return 0;
            }
            button = prevHoveredButton;
        } else if (prevClickedButton != nullptr) {
            if (_button_under_mouse(prevClickedButton, &v58)) {
                if (!(prevClickedButton->flags & BUTTON_FLAG_DISABLED)) {
                    *keyCodePtr = prevClickedButton->mouseEnterEventCode;
                }

                if ((prevClickedButton->flags & BUTTON_FLAG_0x01) && (prevClickedButton->flags & BUTTON_FLAG_CHECKED)) {
                    _button_draw(prevClickedButton, window, prevClickedButton->pressedImage, true, nullptr, true);
                } else {
                    _button_draw(prevClickedButton, window, prevClickedButton->normalImage, true, nullptr, true);
                }

                window->hoveredButton = prevClickedButton;

                _last_button_winID = window->id;

                if (!(prevClickedButton->flags & BUTTON_FLAG_DISABLED)) {
                    if (prevClickedButton->mouseEnterProc != nullptr) {
                        prevClickedButton->mouseEnterProc(prevClickedButton->id, *keyCodePtr);
                        if (!(prevClickedButton->flags & BUTTON_FLAG_0x40)) {
                            *keyCodePtr = -1;
                        }
                    }
                }
                return 0;
            }
        }

        int v25 = _last_button_winID;
        if (_last_button_winID != -1 && _last_button_winID != window->id) {
            Window* v26 = windowGetWindow(_last_button_winID);
            if (v26 != nullptr) {
                _last_button_winID = -1;

                Button* v28 = v26->hoveredButton;
                if (v28 != nullptr) {
                    if (!(v28->flags & BUTTON_FLAG_DISABLED)) {
                        *keyCodePtr = v28->mouseExitEventCode;
                    }

                    if ((v28->flags & BUTTON_FLAG_0x01) && (v28->flags & BUTTON_FLAG_CHECKED)) {
                        _button_draw(v28, v26, v28->pressedImage, true, nullptr, true);
                    } else {
                        _button_draw(v28, v26, v28->normalImage, true, nullptr, true);
                    }

                    v26->clickedButton = nullptr;
                    v26->hoveredButton = nullptr;

                    if (!(v28->flags & BUTTON_FLAG_DISABLED)) {
                        if (v28->mouseExitProc != nullptr) {
                            v28->mouseExitProc(v28->id, *keyCodePtr);
                            if (!(v28->flags & BUTTON_FLAG_0x40)) {
                                *keyCodePtr = -1;
                            }
                        }
                    }
                    return 0;
                }
            }
        }

        ButtonCallback* cb = nullptr;

        while (button != nullptr) {
            if (!(button->flags & BUTTON_FLAG_DISABLED)) {
                rectCopy(&v58, &(button->rect));
                rectOffset(&v58, window->rect.left, window->rect.top);
                if (_button_under_mouse(button, &v58)) {
                    if (!(button->flags & BUTTON_FLAG_DISABLED)) {
                        if ((mouseEvent & MOUSE_EVENT_ANY_BUTTON_DOWN) != 0) {
                            if ((mouseEvent & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0 && (button->flags & BUTTON_FLAG_RIGHT_MOUSE_BUTTON_CONFIGURED) == 0) {
                                button = nullptr;
                                break;
                            }

                            if (button != window->hoveredButton && button != window->clickedButton) {
                                break;
                            }

                            window->clickedButton = button;
                            window->hoveredButton = button;

                            if ((button->flags & BUTTON_FLAG_0x01) != 0) {
                                if ((button->flags & BUTTON_FLAG_0x02) != 0) {
                                    if ((button->flags & BUTTON_FLAG_CHECKED) != 0) {
                                        if (!(button->flags & BUTTON_FLAG_0x04)) {
                                            if (button->buttonGroup != nullptr) {
                                                button->buttonGroup->currChecked--;
                                            }

                                            if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                                                *keyCodePtr = button->leftMouseUpEventCode;
                                                cb = button->leftMouseUpProc;
                                            } else {
                                                *keyCodePtr = button->rightMouseUpEventCode;
                                                cb = button->rightMouseUpProc;
                                            }

                                            button->flags &= ~BUTTON_FLAG_CHECKED;
                                        }
                                    } else {
                                        if (_button_check_group(button) == -1) {
                                            button = nullptr;
                                            break;
                                        }

                                        if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                                            *keyCodePtr = button->lefMouseDownEventCode;
                                            cb = button->leftMouseDownProc;
                                        } else {
                                            *keyCodePtr = button->rightMouseDownEventCode;
                                            cb = button->rightMouseDownProc;
                                        }

                                        button->flags |= BUTTON_FLAG_CHECKED;
                                    }
                                }
                            } else {
                                if (_button_check_group(button) == -1) {
                                    button = nullptr;
                                    break;
                                }

                                if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                                    *keyCodePtr = button->lefMouseDownEventCode;
                                    cb = button->leftMouseDownProc;
                                } else {
                                    *keyCodePtr = button->rightMouseDownEventCode;
                                    cb = button->rightMouseDownProc;
                                }
                            }

                            _button_draw(button, window, button->pressedImage, true, nullptr, true);
                            break;
                        }

                        Button* v49 = window->clickedButton;
                        if (button == v49 && (mouseEvent & MOUSE_EVENT_ANY_BUTTON_UP) != 0) {
                            window->clickedButton = nullptr;
                            window->hoveredButton = v49;

                            if (v49->flags & BUTTON_FLAG_0x01) {
                                if (!(v49->flags & BUTTON_FLAG_0x02)) {
                                    if (v49->flags & BUTTON_FLAG_CHECKED) {
                                        if (!(v49->flags & BUTTON_FLAG_0x04)) {
                                            if (v49->buttonGroup != nullptr) {
                                                v49->buttonGroup->currChecked--;
                                            }

                                            if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                                                *keyCodePtr = button->leftMouseUpEventCode;
                                                cb = button->leftMouseUpProc;
                                            } else {
                                                *keyCodePtr = button->rightMouseUpEventCode;
                                                cb = button->rightMouseUpProc;
                                            }

                                            button->flags &= ~BUTTON_FLAG_CHECKED;
                                        }
                                    } else {
                                        if (_button_check_group(v49) == -1) {
                                            button = nullptr;
                                            _button_draw(v49, window, v49->normalImage, true, nullptr, true);
                                            break;
                                        }

                                        if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                                            *keyCodePtr = v49->lefMouseDownEventCode;
                                            cb = v49->leftMouseDownProc;
                                        } else {
                                            *keyCodePtr = v49->rightMouseDownEventCode;
                                            cb = v49->rightMouseDownProc;
                                        }

                                        v49->flags |= BUTTON_FLAG_CHECKED;
                                    }
                                }
                            } else {
                                if (v49->flags & BUTTON_FLAG_CHECKED) {
                                    if (v49->buttonGroup != nullptr) {
                                        v49->buttonGroup->currChecked--;
                                    }
                                }

                                if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                                    *keyCodePtr = v49->leftMouseUpEventCode;
                                    cb = v49->leftMouseUpProc;
                                } else {
                                    *keyCodePtr = v49->rightMouseUpEventCode;
                                    cb = v49->rightMouseUpProc;
                                }
                            }

                            if (button->hoverImage != nullptr) {
                                _button_draw(button, window, button->hoverImage, true, nullptr, true);
                            } else {
                                _button_draw(button, window, button->normalImage, true, nullptr, true);
                            }
                            break;
                        }
                    }

                    if (window->hoveredButton == nullptr && mouseEvent == 0) {
                        window->hoveredButton = button;
                        if (!(button->flags & BUTTON_FLAG_DISABLED)) {
                            *keyCodePtr = button->mouseEnterEventCode;
                            cb = button->mouseEnterProc;
                        }

                        _button_draw(button, window, button->hoverImage, true, nullptr, true);
                    }
                    break;
                }
            }
            button = button->next;
        }

        if (button != nullptr) {
            if ((button->flags & BUTTON_DRAG_HANDLE) != 0
                && (mouseEvent & MOUSE_EVENT_ANY_BUTTON_DOWN) != 0
                && (mouseEvent & MOUSE_EVENT_ANY_BUTTON_REPEAT) == 0) {
                win_drag(window->id);
                _button_draw(button, window, button->normalImage, true, nullptr, true);
            }
        } else if ((window->flags & WINDOW_DRAGGABLE_BY_BACKGROUND) != 0) {
            v25 |= mouseEvent << 8;
            if ((mouseEvent & MOUSE_EVENT_ANY_BUTTON_DOWN) != 0
                && (mouseEvent & MOUSE_EVENT_ANY_BUTTON_REPEAT) == 0) {
                win_drag(window->id);
            }
        }

        _last_button_winID = window->id;

        if (button != nullptr) {
            if (cb != nullptr) {
                cb(button->id, *keyCodePtr);
                if (!(button->flags & BUTTON_FLAG_0x40)) {
                    *keyCodePtr = -1;
                }
            }
        }

        return 0;
    }

    if (prevHoveredButton != nullptr) {
        *keyCodePtr = prevHoveredButton->mouseExitEventCode;

        unsigned char* data;
        if ((prevHoveredButton->flags & BUTTON_FLAG_0x01) && (prevHoveredButton->flags & BUTTON_FLAG_CHECKED)) {
            data = prevHoveredButton->pressedImage;
        } else {
            data = prevHoveredButton->normalImage;
        }

        _button_draw(prevHoveredButton, window, data, true, nullptr, true);

        window->hoveredButton = nullptr;
    }

    if (*keyCodePtr != -1) {
        _last_button_winID = window->id;

        if ((prevHoveredButton->flags & BUTTON_FLAG_DISABLED) == 0) {
            if (prevHoveredButton->mouseExitProc != nullptr) {
                prevHoveredButton->mouseExitProc(prevHoveredButton->id, *keyCodePtr);
                if (!(prevHoveredButton->flags & BUTTON_FLAG_0x40)) {
                    *keyCodePtr = -1;
                }
            }
        }
        return 0;
    }

    if (prevHoveredButton != nullptr) {
        if ((prevHoveredButton->flags & BUTTON_FLAG_DISABLED) == 0) {
            if (prevHoveredButton->mouseExitProc != nullptr) {
                prevHoveredButton->mouseExitProc(prevHoveredButton->id, *keyCodePtr);
            }
        }
    }

    return -1;
}

// 0x4D9214
bool _button_under_mouse(Button* button, Rect* rect)
{
    if (!_mouse_click_in(rect->left, rect->top, rect->right, rect->bottom)) {
        return false;
    }

    if (button->mask == nullptr) {
        return true;
    }

    int x;
    int y;
    mouseGetPosition(&x, &y);
    x -= rect->left;
    y -= rect->top;

    int width = button->rect.right - button->rect.left + 1;
    return button->mask[width * y + x] != 0;
}

// 0x4D927C
int buttonGetWindowId(int btn)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Window* window;
    if (buttonGetButton(btn, &window) == nullptr) {
        return -1;
    }

    return window->id;
}

// 0x4D92B4
int _win_last_button_winID()
{
    return _last_button_winID;
}

// 0x4D92BC
int buttonDestroy(int btn)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Window* window;
    Button* button = buttonGetButton(btn, &window);
    if (button == nullptr) {
        return -1;
    }

    if (button->prev != nullptr) {
        button->prev->next = button->next;
    } else {
        window->buttonListHead = button->next;
    }

    if (button->next != nullptr) {
        button->next->prev = button->prev;
    }

    windowFill(window->id, button->rect.left, button->rect.top, button->rect.right - button->rect.left + 1, button->rect.bottom - button->rect.top + 1, window->color);

    if (button == window->hoveredButton) {
        window->hoveredButton = nullptr;
    }

    if (button == window->clickedButton) {
        window->clickedButton = nullptr;
    }

    buttonFree(button);

    return 0;
}

// 0x4D9374
void buttonFree(Button* button)
{
    if ((button->flags & BUTTON_FLAG_GRAPHIC) == 0) {
        if (button->normalImage != nullptr) {
            internal_free(button->normalImage);
        }

        if (button->pressedImage != nullptr) {
            internal_free(button->pressedImage);
        }

        if (button->hoverImage != nullptr) {
            internal_free(button->hoverImage);
        }

        if (button->disabledNormalImage != nullptr) {
            internal_free(button->disabledNormalImage);
        }

        if (button->disabledPressedImage != nullptr) {
            internal_free(button->disabledPressedImage);
        }

        if (button->disabledHoverImage != nullptr) {
            internal_free(button->disabledHoverImage);
        }
    }

    ButtonGroup* buttonGroup = button->buttonGroup;
    if (buttonGroup != nullptr) {
        for (int index = 0; index < buttonGroup->buttonsLength; index++) {
            if (button == buttonGroup->buttons[index]) {
                for (; index < buttonGroup->buttonsLength - 1; index++) {
                    buttonGroup->buttons[index] = buttonGroup->buttons[index + 1];
                }

                buttonGroup->buttonsLength--;

                break;
            }
        }
    }

    internal_free(button);
}

// NOTE: Inlined.
//
// 0x4D9458
static int button_new_id()
{
    int btn;

    btn = 1;
    while (buttonGetButton(btn, nullptr) != nullptr) {
        btn++;
    }

    return btn;
}

// 0x4D9474
int buttonEnable(int btn)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Window* window;
    Button* button = buttonGetButton(btn, &window);
    if (button == nullptr) {
        return -1;
    }

    if ((button->flags & BUTTON_FLAG_DISABLED) != 0) {
        button->flags &= ~BUTTON_FLAG_DISABLED;
        _button_draw(button, window, button->currentImage, true, nullptr, false);
    }

    return 0;
}

// 0x4D94D0
int buttonDisable(int btn)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Window* window;
    Button* button = buttonGetButton(btn, &window);
    if (button == nullptr) {
        return -1;
    }

    if ((button->flags & BUTTON_FLAG_DISABLED) == 0) {
        button->flags |= BUTTON_FLAG_DISABLED;

        _button_draw(button, window, button->currentImage, true, nullptr, false);

        if (button == window->hoveredButton) {
            if (window->hoveredButton->mouseExitEventCode != -1) {
                enqueueInputEvent(window->hoveredButton->mouseExitEventCode);
                window->hoveredButton = nullptr;
            }
        }
    }

    return 0;
}

// 0x4D9554
int _win_set_button_rest_state(int btn, bool checked, int flags)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Window* window;
    Button* button = buttonGetButton(btn, &window);
    if (button == nullptr) {
        return -1;
    }

    if ((button->flags & BUTTON_FLAG_0x01) != 0) {
        int keyCode = -1;

        if ((button->flags & BUTTON_FLAG_CHECKED) != 0) {
            if (!checked) {
                button->flags &= ~BUTTON_FLAG_CHECKED;

                if ((flags & 0x02) == 0) {
                    _button_draw(button, window, button->normalImage, true, nullptr, false);
                }

                if (button->buttonGroup != nullptr) {
                    button->buttonGroup->currChecked--;
                }

                keyCode = button->leftMouseUpEventCode;
            }
        } else {
            if (checked) {
                button->flags |= BUTTON_FLAG_CHECKED;

                if ((flags & 0x02) == 0) {
                    _button_draw(button, window, button->pressedImage, true, nullptr, false);
                }

                if (button->buttonGroup != nullptr) {
                    button->buttonGroup->currChecked++;
                }

                keyCode = button->lefMouseDownEventCode;
            }
        }

        if (keyCode != -1) {
            if ((flags & 0x01) != 0) {
                enqueueInputEvent(keyCode);
            }
        }
    }

    return 0;
}

// 0x4D962C
int _win_group_check_buttons(int buttonCount, int* btns, int maxChecked, RadioButtonCallback* func)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (buttonCount >= BUTTON_GROUP_BUTTON_LIST_CAPACITY) {
        return -1;
    }

    for (int groupIndex = 0; groupIndex < BUTTON_GROUP_LIST_CAPACITY; groupIndex++) {
        ButtonGroup* buttonGroup = &(gButtonGroups[groupIndex]);
        if (buttonGroup->buttonsLength == 0) {
            buttonGroup->currChecked = 0;

            for (int buttonIndex = 0; buttonIndex < buttonCount; buttonIndex++) {
                Button* button = buttonGetButton(btns[buttonIndex], nullptr);
                if (button == nullptr) {
                    return -1;
                }

                buttonGroup->buttons[buttonIndex] = button;

                button->buttonGroup = buttonGroup;

                if ((button->flags & BUTTON_FLAG_CHECKED) != 0) {
                    buttonGroup->currChecked++;
                }
            }

            buttonGroup->buttonsLength = buttonCount;
            buttonGroup->maxChecked = maxChecked;
            buttonGroup->func = func;
            return 0;
        }
    }

    return -1;
}

// 0x4D96EC
int _win_group_radio_buttons(int count, int* btns)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    if (_win_group_check_buttons(count, btns, 1, nullptr) == -1) {
        return -1;
    }

    Button* button = buttonGetButton(btns[0], nullptr);
    ButtonGroup* buttonGroup = button->buttonGroup;

    for (int index = 0; index < buttonGroup->buttonsLength; index++) {
        Button* v1 = buttonGroup->buttons[index];
        v1->flags |= BUTTON_FLAG_RADIO;
    }

    return 0;
}

// 0x4D9744
int _button_check_group(Button* button)
{
    if (button->buttonGroup == nullptr) {
        return 0;
    }

    if ((button->flags & BUTTON_FLAG_RADIO) != 0) {
        if (button->buttonGroup->currChecked > 0) {
            for (int index = 0; index < button->buttonGroup->buttonsLength; index++) {
                Button* v1 = button->buttonGroup->buttons[index];
                if ((v1->flags & BUTTON_FLAG_CHECKED) != 0) {
                    v1->flags &= ~BUTTON_FLAG_CHECKED;

                    Window* window;
                    buttonGetButton(v1->id, &window);
                    _button_draw(v1, window, v1->normalImage, true, nullptr, true);

                    if (v1->leftMouseUpProc != nullptr) {
                        v1->leftMouseUpProc(v1->id, v1->leftMouseUpEventCode);
                    }
                }
            }
        }

        if ((button->flags & BUTTON_FLAG_CHECKED) == 0) {
            button->buttonGroup->currChecked++;
        }

        return 0;
    }

    if (button->buttonGroup->currChecked < button->buttonGroup->maxChecked) {
        if ((button->flags & BUTTON_FLAG_CHECKED) == 0) {
            button->buttonGroup->currChecked++;
        }

        return 0;
    }

    if (button->buttonGroup->func != nullptr) {
        button->buttonGroup->func(button->id);
    }

    return -1;
}

// 0x4D9808
void _button_draw(Button* button, Window* window, unsigned char* data, bool draw, Rect* bound, bool sound)
{
    unsigned char* previousImage = nullptr;
    if (data != nullptr) {
        Rect v2;
        rectCopy(&v2, &(button->rect));
        rectOffset(&v2, window->rect.left, window->rect.top);

        Rect v3;
        if (bound != nullptr) {
            if (rectIntersection(&v2, bound, &v2) == -1) {
                return;
            }

            rectCopy(&v3, &v2);
            rectOffset(&v3, -window->rect.left, -window->rect.top);
        } else {
            rectCopy(&v3, &(button->rect));
        }

        if (data == button->normalImage && (button->flags & BUTTON_FLAG_CHECKED)) {
            data = button->pressedImage;
        }

        if (button->flags & BUTTON_FLAG_DISABLED) {
            if (data == button->normalImage) {
                data = button->disabledNormalImage;
            } else if (data == button->pressedImage) {
                data = button->disabledPressedImage;
            } else if (data == button->hoverImage) {
                data = button->disabledHoverImage;
            }
        } else {
            if (data == button->disabledNormalImage) {
                data = button->normalImage;
            } else if (data == button->disabledPressedImage) {
                data = button->pressedImage;
            } else if (data == button->disabledHoverImage) {
                data = button->hoverImage;
            }
        }

        if (data) {
            if (!draw) {
                int width = button->rect.right - button->rect.left + 1;
                if ((button->flags & BUTTON_FLAG_TRANSPARENT) != 0) {
                    blitBufferToBufferTrans(
                        data + (v3.top - button->rect.top) * width + v3.left - button->rect.left,
                        v3.right - v3.left + 1,
                        v3.bottom - v3.top + 1,
                        width,
                        window->buffer + window->width * v3.top + v3.left,
                        window->width);
                } else {
                    blitBufferToBuffer(
                        data + (v3.top - button->rect.top) * width + v3.left - button->rect.left,
                        v3.right - v3.left + 1,
                        v3.bottom - v3.top + 1,
                        width,
                        window->buffer + window->width * v3.top + v3.left,
                        window->width);
                }
            }

            previousImage = button->currentImage;
            button->currentImage = data;

            if (draw) {
                _GNW_win_refresh(window, &v2, nullptr);
            }
        }
    }

    if (sound) {
        if (previousImage != data) {
            if (data == button->pressedImage && button->pressSoundFunc != nullptr) {
                button->pressSoundFunc(button->id, button->lefMouseDownEventCode);
            } else if (data == button->normalImage && button->releaseSoundFunc != nullptr) {
                button->releaseSoundFunc(button->id, button->leftMouseUpEventCode);
            }
        }
    }
}

// 0x4D9A58
void _GNW_button_refresh(Window* window, Rect* rect)
{
    Button* button = window->buttonListHead;
    if (button != nullptr) {
        while (button->next != nullptr) {
            button = button->next;
        }
    }

    while (button != nullptr) {
        _button_draw(button, window, button->currentImage, false, rect, false);
        button = button->prev;
    }
}

// 0x4D9AA0
int _win_button_press_and_release(int btn)
{
    if (!gWindowSystemInitialized) {
        return -1;
    }

    Window* window;
    Button* button = buttonGetButton(btn, &window);
    if (button == nullptr) {
        return -1;
    }

    _button_draw(button, window, button->pressedImage, true, nullptr, true);

    if (button->leftMouseDownProc != nullptr) {
        button->leftMouseDownProc(btn, button->lefMouseDownEventCode);

        if ((button->flags & BUTTON_FLAG_0x40) != 0) {
            enqueueInputEvent(button->lefMouseDownEventCode);
        }
    } else {
        if (button->lefMouseDownEventCode != -1) {
            enqueueInputEvent(button->lefMouseDownEventCode);
        }
    }

    _button_draw(button, window, button->normalImage, true, nullptr, true);

    if (button->leftMouseUpProc != nullptr) {
        button->leftMouseUpProc(btn, button->leftMouseUpEventCode);

        if ((button->flags & BUTTON_FLAG_0x40) != 0) {
            enqueueInputEvent(button->leftMouseUpEventCode);
        }
    } else {
        if (button->leftMouseUpEventCode != -1) {
            enqueueInputEvent(button->leftMouseUpEventCode);
        }
    }

    return 0;
}

unsigned char* windowGetVirtualScreenBuffer()
{
    return _screen_buffer;
}

int windowGetVirtualScreenPitch()
{
    return _screen_buffer != nullptr ? _screen_buffer_pitch : 0;
}

bool windowIsVirtualScreenEnabled()
{
    return gVirtualScreenEnabled;
}

void windowPresentVirtualScreen()
{
    windowRefreshPhysicalTrueColorBuffers();

    if (!gVirtualScreenEnabled || !gVirtualScreenDirty || _screen_buffer == nullptr) {
        return;
    }

    windowResetPresentStats();

    Rect rect = gVirtualScreenDirtyRect;
    virtualScreenResetDirty();

    const Rect& bounds = displayScalerGetLogicalBounds();
    if (rectIntersection(&rect, &bounds, &rect) == -1) {
        return;
    }

    int width = rectGetWidth(&rect);
    int height = rectGetHeight(&rect);
    if (width <= 0 || height <= 0) {
        return;
    }

    logVirtualScreenRectStats(rect);
    windowAccumulateLogicalRectStats(rect);

    Rect physicalDirtyRect = displayScalerLogicalToPhysical(rect);
    windowAccumulatePhysicalRectStats(physicalDirtyRect);

    blitIndexedRectToTexture(_screen_buffer, _screen_buffer_pitch, rect);

    int overlayPixels = windowCompositeTrueColorOverlays(rect);
    if (overlayPixels > 0 && diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "hd_overlay virtual=(%d,%d %dx%d) pixels_overridden=%d",
            rect.left,
            rect.top,
            width,
            height,
            overlayPixels);
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        const Rect& viewport = displayScalerGetPhysicalViewport();
        diagnosticsLog(
            DiagnosticsLevel::Trace,
            "SCALER",
            "virtual_present virtual=(%d,%d %dx%d) viewport=(%d,%d %dx%d)",
            rect.left,
            rect.top,
            width,
            height,
            viewport.left,
            viewport.top,
            rectGetWidth(&viewport),
            rectGetHeight(&viewport));
    }

    windowLogPresentStats(rect, physicalDirtyRect);
}

void windowVirtualScreenInvalidateAll()
{
    virtualScreenInvalidateAll();
}

void windowRefreshPhysicalTrueColorBuffers()
{
    windowSyncPhysicalTrueColorBuffersIfNeeded();
}

uint32_t windowGetPhysicalTrueColorOverlayRevision()
{
    return gPhysicalTrueColorOverlayRevision;
}

// Legacy true-color compatibility layer ------------------------------------

void windowTrueColorSetPaletteBaseline(const unsigned char* /*palette*/)
{
    // No-op until the true-color compositor returns.
}

bool isTrueColorRendererActive()
{
    return gVirtualScreenEnabled;
}

unsigned char* windowGetScreenBuffer()
{
    return _screen_buffer;
}

int windowGetScreenPitch()
{
    return _screen_buffer != nullptr ? _screen_buffer_pitch : 0;
}

bool windowHasTrueColorOverlay(int win)
{
    if (!gVirtualScreenEnabled) {
        return false;
    }

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return false;
    }

    return window->trueColorOverlay != nullptr && window->trueColorMask != nullptr;
}

uint32_t* windowGetTrueColorOverlay(int win)
{
    if (!windowHasTrueColorOverlay(win)) {
        return nullptr;
    }

    Window* window = windowGetWindow(win);
    return window != nullptr ? window->trueColorOverlay : nullptr;
}

unsigned char* windowGetTrueColorMask(int win)
{
    if (!windowHasTrueColorOverlay(win)) {
        return nullptr;
    }

    Window* window = windowGetWindow(win);
    return window != nullptr ? window->trueColorMask : nullptr;
}

bool windowHasPhysicalTrueColorOverlay(int win)
{
    if (!gVirtualScreenEnabled) {
        return false;
    }

    windowRefreshPhysicalTrueColorBuffers();

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return false;
    }

    return window->trueColorPhysicalOverlay != nullptr && window->trueColorPhysicalMask != nullptr && window->trueColorPhysicalPitch > 0;
}

bool windowGetPhysicalTrueColorOverlay(int win, WindowPhysicalTrueColorBuffer* outBuffer)
{
    windowRefreshPhysicalTrueColorBuffers();

    if (!windowHasPhysicalTrueColorOverlay(win)) {
        return false;
    }

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return false;
    }

    if (outBuffer != nullptr) {
        outBuffer->pixels = window->trueColorPhysicalOverlay;
        outBuffer->mask = window->trueColorPhysicalMask;
        outBuffer->pitch = window->trueColorPhysicalPitch;
        outBuffer->width = window->trueColorPhysicalWidth;
        outBuffer->height = window->trueColorPhysicalHeight;
        outBuffer->viewport = window->trueColorPhysicalViewport;
    }

    return true;
}

void windowClearTrueColorRegion(int win, int left, int top, int width, int height)
{
    windowRefreshPhysicalTrueColorBuffers();

    if (!windowHasTrueColorOverlay(win) || width <= 0 || height <= 0) {
        return;
    }

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return;
    }

    int startX = std::clamp(left, 0, window->width);
    int startY = std::clamp(top, 0, window->height);
    int endX = std::clamp(left + width, 0, window->width);
    int endY = std::clamp(top + height, 0, window->height);

    if (startX >= endX || startY >= endY) {
        return;
    }

    int rowWidth = endX - startX;
    for (int y = startY; y < endY; y++) {
        int offset = y * window->width + startX;
        memset(window->trueColorMask + offset, 0, rowWidth);
        memset(window->trueColorOverlay + offset, 0, rowWidth * sizeof(uint32_t));
    }

    if (window->trueColorPhysicalOverlay != nullptr && window->trueColorPhysicalMask != nullptr && window->trueColorPhysicalPitch > 0 && window->trueColorPhysicalWidth > 0 && window->trueColorPhysicalHeight > 0) {
        Rect logicalRect;
        logicalRect.left = window->rect.left + startX;
        logicalRect.top = window->rect.top + startY;
        logicalRect.right = logicalRect.left + (rowWidth - 1);
        logicalRect.bottom = logicalRect.top + (endY - startY - 1);

        Rect physicalRect = displayScalerLogicalToPhysical(logicalRect);
        Rect viewport = window->trueColorPhysicalViewport;
        if (rectIntersection(&physicalRect, &viewport, &physicalRect) != -1) {
            int physicalWidth = rectGetWidth(&physicalRect);
            int physicalHeight = rectGetHeight(&physicalRect);
            if (physicalWidth > 0 && physicalHeight > 0) {
                int destLeft = physicalRect.left - viewport.left;
                int destTop = physicalRect.top - viewport.top;
                for (int row = 0; row < physicalHeight; row++) {
                    uint32_t* overlayRow = window->trueColorPhysicalOverlay + (destTop + row) * window->trueColorPhysicalPitch + destLeft;
                    unsigned char* maskRow = window->trueColorPhysicalMask + (destTop + row) * window->trueColorPhysicalPitch + destLeft;
                    memset(overlayRow, 0, physicalWidth * sizeof(uint32_t));
                    memset(maskRow, 0, physicalWidth);
                }
            }
        }
    }
}

void windowDebugStampMissingHdGlyph(int win, int left, int top, int width, int height)
{
    if (!settings.debug.hd_missing_watermark) {
        return;
    }

    if (!windowHasTrueColorOverlay(win)) {
        return;
    }

    Window* window = windowGetWindow(win);
    if (window == nullptr) {
        return;
    }

    int regionLeft = std::clamp(left, 0, window->width);
    int regionTop = std::clamp(top, 0, window->height);
    int regionRight = std::clamp(left + width, 0, window->width);
    int regionBottom = std::clamp(top + height, 0, window->height);
    if (regionLeft >= regionRight || regionTop >= regionBottom) {
        return;
    }

    constexpr int glyphWidth = 6;
    constexpr int glyphHeight = 6;
    static const unsigned char glyph[glyphHeight][glyphWidth] = {
        { 1, 0, 0, 0, 0, 1 },
        { 1, 1, 0, 0, 1, 1 },
        { 1, 0, 1, 1, 0, 1 },
        { 1, 0, 1, 1, 0, 1 },
        { 1, 1, 0, 0, 1, 1 },
        { 1, 0, 0, 0, 0, 1 },
    };

    int regionWidth = regionRight - regionLeft;
    int regionHeight = regionBottom - regionTop;
    int drawWidth = std::min(glyphWidth, regionWidth);
    int drawHeight = std::min(glyphHeight, regionHeight);
    if (drawWidth <= 0 || drawHeight <= 0) {
        return;
    }

    int glyphLeft = regionLeft + (regionWidth - drawWidth) / 2;
    int glyphTop = regionTop + (regionHeight - drawHeight) / 2;

    uint32_t* overlayRow = window->trueColorOverlay + glyphTop * window->width + glyphLeft;
    unsigned char* maskRow = window->trueColorMask + glyphTop * window->width + glyphLeft;
    const uint32_t glyphColor = 0xFFFF40FF;

    for (int y = 0; y < drawHeight; y++) {
        const unsigned char* glyphRow = glyph[y];
        uint32_t* overlayPixel = overlayRow;
        unsigned char* maskPixel = maskRow;
        for (int x = 0; x < drawWidth; x++) {
            if (glyphRow[x] != 0) {
                *overlayPixel = glyphColor;
                *maskPixel = 1;
            }
            overlayPixel++;
            maskPixel++;
        }

        overlayRow += window->width;
        maskRow += window->width;
    }
}

} // namespace fallout
