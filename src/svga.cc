#include "svga.h"

#include <limits.h>
#include <string.h>

#include <SDL.h>

#include "config.h"
#include "debug.h"
#include "draw.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"
#include "display_scale.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect
Rect _scr_size;

// When non-zero, overrides the created SDL window size independent of logical size.
static int gRequestedWindowW = 0;
static int gRequestedWindowH = 0;

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

// High-res composition mode globals
bool gHiResEnabled = false;
int gHiResScale = 1; // 1 = off
bool gHiResOverlayTransparent = true;
bool gHiResDebugOverlayMarker = false;
static SDL_Surface* gHiResBackground = nullptr; // owned

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
    bool fitToWindow = false; // Preserve 640x480 logical size and scale to window
    // Reset hires defaults each init
    gHiResEnabled = false;
    gHiResScale = 1;

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

            // Cache original requested window size from config before any scaling logic.
            int cfgWindowW = width;
            int cfgWindowH = height;

            bool windowed;
            if (configGetBool(&resolutionConfig, "MAIN", "WINDOWED", &windowed)) {
                fullscreen = !windowed;
            }

            // New option: When enabled, force logical 640x480 and scale to fit window (preserve aspect).
            configGetBool(&resolutionConfig, "MAIN", "FIT_TO_WINDOW", &fitToWindow);

            // Broader high-res mode: render to a higher resolution truecolor backbuffer
            bool hiresMode = false;
            if (configGetBool(&resolutionConfig, "MAIN", "HIRES_MODE", &hiresMode)) {
                gHiResEnabled = hiresMode;
            }
            int hiresScale = 0;
            if (configGetInt(&resolutionConfig, "MAIN", "HIRES_SCALE", &hiresScale)) {
                gHiResScale = hiresScale;
            }
            if (!gHiResEnabled) {
                gHiResScale = 1;
            }
            if (gHiResScale < 1) gHiResScale = 1;
            if (gHiResScale > 4) gHiResScale = 4;

            // Control whether 8-bit overlay uses palette index 0 as transparent over hi-res background.
            // Default: true (preserve background through cleared regions). Set to false for debugging if
            // legacy assets appear invisible due to index 0 usage.
            bool overlayTransparent = true;
            if (configGetBool(&resolutionConfig, "MAIN", "HIRES_OVERLAY_TRANSPARENT", &overlayTransparent)) {
                gHiResOverlayTransparent = overlayTransparent;
            } else {
                gHiResOverlayTransparent = true;
            }

            // Debug aid: draw a small marker into the 8-bit overlay to confirm it is being composed.
            bool debugOverlayMarker = false;
            if (configGetBool(&resolutionConfig, "MAIN", "DEBUG_OVERLAY_MARKER", &debugOverlayMarker)) {
                gHiResDebugOverlayMarker = debugOverlayMarker;
            } else {
                gHiResDebugOverlayMarker = false;
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
        }
        configFree(&resolutionConfig);
    }

    // Apply FIT_TO_WINDOW by keeping logical size at 640x480 and requesting
    // the window size from SCR_WIDTH/SCR_HEIGHT (via gRequestedWindowW/H).
    if (fitToWindow) {
        // Remember requested window size from config (pre-scaling values).
        // If config wasn't read, these remain 0 and fallback path will be used.
        // Note: If SCALE_2X was set, FIT_TO_WINDOW takes precedence.
        // Re-read from resolutionConfig is not possible here, so rely on cached cfgWindowW/H.
        // If not available, default to current width/height * scale (best-effort).
        if (gRequestedWindowW == 0 || gRequestedWindowH == 0) {
            // We cannot directly access cfgWindowW/H here; recompute best-effort.
            // Since width/height may have been divided by scale above, multiply back.
            gRequestedWindowW = width * scale;
            gRequestedWindowH = height * scale;
        }
        // Use base logical size.
        width = 640;
        height = 480;
        scale = 1;
    } else {
        gRequestedWindowW = 0;
        gRequestedWindowH = 0;
    }

    if (_GNW95_init_window(width, height, fullscreen, scale) == -1) {
        return -1;
    }

    if (directDrawInit(width, height, bpp) == -1) {
        return -1;
    }

    _scr_size.left = 0;
    _scr_size.top = 0;
    _scr_size.right = width - 1;
    _scr_size.bottom = height - 1;

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

        int windowW = (gRequestedWindowW > 0 ? gRequestedWindowW : (width * scale));
        int windowH = (gRequestedWindowH > 0 ? gRequestedWindowH : (height * scale));
        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, windowW, windowH, windowFlags);
        if (gSdlWindow == nullptr) {
            return -1;
        }

        if (!createRenderer(width, height)) {
            destroyRenderer();

            SDL_DestroyWindow(gSdlWindow);
            gSdlWindow = nullptr;

            return -1;
        }

        // Initialize logical/physical scale mapping for rendering and input.
        int winW = 0, winH = 0;
        SDL_GetWindowSize(gSdlWindow, &winW, &winH);
        displayScaleInit(width, height, winW, winH);
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
        if (!gHiResEnabled) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
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
        if (!gHiResEnabled) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
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
    blitBufferToBuffer(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    if (!gHiResEnabled) {
        SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
    }
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

    if (!gHiResEnabled) {
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    }
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&_scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&_scr_size);
}

int screenGetVisibleHeight()
{
    int windowBottomMargin = 0;

    if (!gInterfaceBarMode) {
        windowBottomMargin = INTERFACE_BAR_HEIGHT;
    }
    return screenGetHeight() - windowBottomMargin;
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == nullptr) {
        return false;
    }

    int outW = width * (gHiResEnabled ? gHiResScale : 1);
    int outH = height * (gHiResEnabled ? gHiResScale : 1);

    if (SDL_RenderSetLogicalSize(gSdlRenderer, outW, outH) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, outW, outH);
    if (gSdlTexture == nullptr) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, outW, outH, SDL_BITSPERPIXEL(format), format);
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

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());

    // Recompute scale mapping on resize.
    if (gSdlWindow != nullptr) {
        int winW = 0, winH = 0;
        SDL_GetWindowSize(gSdlWindow, &winW, &winH);
        displayScaleInit(screenGetWidth(), screenGetHeight(), winW, winH);
    }
}

void renderPresent()
{
    if (gHiResEnabled) {
        // Compose: background (if any) + scaled 8-bit screen
        // Clear to black first
        SDL_FillRect(gSdlTextureSurface, nullptr, SDL_MapRGB(gSdlTextureSurface->format, 0, 0, 0));

        if (gHiResBackground != nullptr) {
            // Fit background to texture surface, preserving aspect, centered
            SDL_Rect dst;
            int tw = gSdlTextureSurface->w, th = gSdlTextureSurface->h;
            int bw = gHiResBackground->w, bh = gHiResBackground->h;
            // Compute scale to fit inside
            double sx = (double)tw / (double)bw;
            double sy = (double)th / (double)bh;
            double s = sx < sy ? sx : sy;
            int dw = (int)(bw * s);
            int dh = (int)(bh * s);
            dst.x = (tw - dw) / 2;
            dst.y = (th - dh) / 2;
            dst.w = dw;
            dst.h = dh;
            SDL_BlitScaled(gHiResBackground, nullptr, gSdlTextureSurface, &dst);
        }

        // Scale 8-bit logical surface to the output surface; when a background is set and
        // overlay transparency is enabled, treat palette index 0 as transparent to let it show.
        if (gHiResBackground != nullptr && gHiResOverlayTransparent) {
            SDL_SetColorKey(gSdlSurface, SDL_TRUE, 0);
        } else {
            SDL_SetColorKey(gSdlSurface, SDL_FALSE, 0);
        }

        // Optional debug marker: draw a small square into the 8-bit overlay so we can verify it appears.
        if (gHiResDebugOverlayMarker && gSdlSurface && gSdlSurface->format && gSdlSurface->pixels) {
            // Draw a 40x40 block in the top-left corner with a bright index (250) to stand out.
            int w = gSdlSurface->w;
            int h = gSdlSurface->h;
            int bw = (w >= 40 ? 40 : w);
            int bh = (h >= 40 ? 40 : h);
            for (int y = 0; y < bh; ++y) {
                unsigned char* row = (unsigned char*)gSdlSurface->pixels + y * gSdlSurface->pitch;
                memset(row, 250, bw);
            }
        }
        SDL_Rect out = { 0, 0, gSdlTextureSurface->w, gSdlTextureSurface->h };

        // More robust path: convert overlay to the destination pixel format before scaling/blitting.
        // This avoids potential issues with palettized->truecolor scaling on some SDL builds.
        SDL_Surface* srcConv = SDL_ConvertSurfaceFormat(gSdlSurface, gSdlTextureSurface->format->format, 0);
        if (srcConv != nullptr) {
            // If we want transparency, set a colorkey matching index-0 color in converted space.
            if (gHiResBackground != nullptr && gHiResOverlayTransparent) {
                // Map the first palette color (index 0) from gSdlSurface into converted format.
                SDL_Color keyCol = {0, 0, 0, 255};
                if (gSdlSurface->format && gSdlSurface->format->palette) {
                    keyCol = gSdlSurface->format->palette->colors[0];
                }
                Uint32 mappedKey = SDL_MapRGB(srcConv->format, keyCol.r, keyCol.g, keyCol.b);
                SDL_SetColorKey(srcConv, SDL_TRUE, mappedKey);
            } else {
                SDL_SetColorKey(srcConv, SDL_FALSE, 0);
            }
            if (SDL_BlitScaled(srcConv, nullptr, gSdlTextureSurface, &out) != 0) {
                debugPrint("HiRes: SDL_BlitScaled overlay(conv) failed: %s\n", SDL_GetError());
            }
            SDL_FreeSurface(srcConv);
        } else {
            if (SDL_BlitScaled(gSdlSurface, nullptr, gSdlTextureSurface, &out) != 0) {
                debugPrint("HiRes: SDL_BlitScaled overlay failed: %s\n", SDL_GetError());
            }
        }
    }

    SDL_UpdateTexture(gSdlTexture, nullptr, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    SDL_RenderPresent(gSdlRenderer);
}

void hiResClearBackground()
{
    if (gHiResBackground != nullptr) {
        SDL_FreeSurface(gHiResBackground);
        gHiResBackground = nullptr;
    }
}

void hiResSetBackground(SDL_Surface* surface)
{
    hiResClearBackground();
    gHiResBackground = surface; // take ownership
}

} // namespace fallout
