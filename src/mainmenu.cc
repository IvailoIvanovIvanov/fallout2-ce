#include "mainmenu.h"

#include <ctype.h>

#include "art.h"
#include "color.h"
#include "draw.h"
#include "art_png_loader.h"
#include "art_texture.h"
#include "game.h"
#include "game_sound.h"
#include "memory.h"
#include "debug.h"
#include "input.h"
#include "kb.h"
#include "mouse.h"
#include "palette.h"
#include "preferences.h"
#include "sfall_config.h"
#include "config.h"
#include "svga.h"
#include "text_font.h"
#include "version.h"
#include "window_manager.h"

#include <SDL.h>

namespace fallout {

#define MAIN_MENU_WINDOW_WIDTH 640
#define MAIN_MENU_WINDOW_HEIGHT 480

typedef enum MainMenuButton {
    MAIN_MENU_BUTTON_INTRO,
    MAIN_MENU_BUTTON_NEW_GAME,
    MAIN_MENU_BUTTON_LOAD_GAME,
    MAIN_MENU_BUTTON_OPTIONS,
    MAIN_MENU_BUTTON_CREDITS,
    MAIN_MENU_BUTTON_EXIT,
    MAIN_MENU_BUTTON_COUNT,
} MainMenuButton;

static int main_menu_fatal_error();
static void main_menu_play_sound(const char* fileName);

// 0x5194F0
static int gMainMenuWindow = -1;

// 0x5194F4
static unsigned char* gMainMenuWindowBuffer = nullptr;

// 0x519504
static bool _in_main_menu = false;

// 0x519508
static bool gMainMenuWindowInitialized = false;

// 0x51950C
static unsigned int gMainMenuScreensaverDelay = 120000;

// 0x519510
static const int gMainMenuButtonKeyBindings[MAIN_MENU_BUTTON_COUNT] = {
    KEY_LOWERCASE_I, // intro
    KEY_LOWERCASE_N, // new game
    KEY_LOWERCASE_L, // load game
    KEY_LOWERCASE_O, // options
    KEY_LOWERCASE_C, // credits
    KEY_LOWERCASE_E, // exit
};

// 0x519528
static const int _return_values[MAIN_MENU_BUTTON_COUNT] = {
    MAIN_MENU_INTRO,
    MAIN_MENU_NEW_GAME,
    MAIN_MENU_LOAD_GAME,
    MAIN_MENU_OPTIONS,
    MAIN_MENU_CREDITS,
    MAIN_MENU_EXIT,
};

// 0x614840
static int gMainMenuButtons[MAIN_MENU_BUTTON_COUNT];

// 0x614858
static bool gMainMenuWindowHidden;

static FrmImage _mainMenuBackgroundFrmImage;
static FrmImage _mainMenuButtonNormalFrmImage;
static FrmImage _mainMenuButtonPressedFrmImage;
// Optional PNG overrides for main menu buttons (indexed, logical size)
static unsigned char* _mainMenuButtonNormalPng = nullptr;
static unsigned char* _mainMenuButtonPressedPng = nullptr;
static int _mainMenuButtonPngW = 0;
static int _mainMenuButtonPngH = 0;
// Control flags (legacy config). Defaults ensure icons and labels are visible and PNGs are preferred when available.
static bool _hideMainMenuIcons = false;
static bool _hideMainMenuLabels = false;
static bool _usePngButtons = true;

// 0x481650
int mainMenuWindowInit()
{
    int fid;
    MessageListItem msg;
    int len;

    if (gMainMenuWindowInitialized) {
        return 0;
    }

    colorPaletteLoad("color.pal");

    // Read optional UI controls from f2_res.ini
    {
        Config cfg;
        if (configInit(&cfg)) {
            if (configRead(&cfg, "f2_res.ini", false)) {
                // When true and a hi-res background is active, hide legacy circular icons so
                // only background UI is visible. Default false (show icons).
                bool hideIcons = false;
                if (configGetBool(&cfg, "MAIN", "HIRES_HIDE_MAINMENU_ICONS", &hideIcons)) {
                    _hideMainMenuIcons = hideIcons;
                } else {
                    _hideMainMenuIcons = false;
                }

                bool hideLabels = false;
                if (configGetBool(&cfg, "MAIN", "HIRES_HIDE_MAINMENU_LABELS", &hideLabels)) {
                    _hideMainMenuLabels = hideLabels;
                } else {
                    _hideMainMenuLabels = false;
                }

                bool usePngButtons = true;
                if (configGetBool(&cfg, "MAIN", "HIRES_BUTTONS_USE_PNG", &usePngButtons)) {
                    _usePngButtons = usePngButtons;
                } else {
                    // Prefer PNGs by default when present.
                    _usePngButtons = true;
                }
            }
            configFree(&cfg);
        }
    }

    int mainMenuWindowX = (screenGetWidth() - MAIN_MENU_WINDOW_WIDTH) / 2;
    int mainMenuWindowY = (screenGetHeight() - MAIN_MENU_WINDOW_HEIGHT) / 2;
    gMainMenuWindow = windowCreate(mainMenuWindowX,
        mainMenuWindowY,
        MAIN_MENU_WINDOW_WIDTH,
        MAIN_MENU_WINDOW_HEIGHT,
        0,
        WINDOW_HIDDEN | WINDOW_MOVE_ON_TOP);
    if (gMainMenuWindow == -1) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }

    gMainMenuWindowBuffer = windowGetBuffer(gMainMenuWindow);

    // mainmenu.frm
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 140, 0, 0, 0);
    // Prefer PNG override for main menu background.
    unsigned char* bgPng = nullptr;
    int bgW = 0, bgH = 0;
    // Debug: report if PNG override exists and expected logical size.
    {
        const ArtTextureMeta* meta = artTextureGetMeta(backgroundFid);
        const char* frmPathDbg = artBuildFilePath(backgroundFid);
        if (frmPathDbg && *frmPathDbg) {
            char pngPathDbg[COMPAT_MAX_PATH];
            strncpy(pngPathDbg, frmPathDbg, sizeof(pngPathDbg) - 1);
            pngPathDbg[sizeof(pngPathDbg) - 1] = '\0';
            char* dot = strrchr(pngPathDbg, '.');
            if (dot) strcpy(dot, ".png"); else if (strlen(pngPathDbg) + 4 < sizeof(pngPathDbg)) strcat(pngPathDbg, ".png");
            if (meta) {
                debugPrint("MM: PNG override detected FRM=\"%s\" PNG=\"%s\" size=%dx%d logical=%dx%d @%dx\n",
                    frmPathDbg, pngPathDbg, meta->pngWidth, meta->pngHeight, meta->logicalWidth, meta->logicalHeight, meta->sourceScale);
            } else {
                debugPrint("MM: No PNG override found FRM=\"%s\" PNG=\"%s\"\n", frmPathDbg, pngPathDbg);
            }
        }
    }
    bool usedHiResBackground = false;
    bool usedHiResBackgroundFromPng = false;
#ifdef HAVE_SDL2_IMAGE
    if (gHiResEnabled) {
        SDL_Surface* bgSurface = artPngLoadSurface(backgroundFid);
        if (bgSurface != nullptr) {
            hiResSetBackground(bgSurface);
            usedHiResBackground = true;
            usedHiResBackgroundFromPng = true;
            // In hi-res mode, do not draw the FRM background; clear to palette index 0
            memset(gMainMenuWindowBuffer, 0, 640 * 480);
            const char* frmPath = artBuildFilePath(backgroundFid);
            if (frmPath && *frmPath) {
                char pngPath[COMPAT_MAX_PATH];
                strncpy(pngPath, frmPath, sizeof(pngPath) - 1);
                pngPath[sizeof(pngPath) - 1] = '\0';
                char* dot = strrchr(pngPath, '.');
                if (dot) strcpy(dot, ".png"); else if (strlen(pngPath) + 4 < sizeof(pngPath)) strcat(pngPath, ".png");
                debugPrint("MM: hi-res background set FRM=\"%s\" PNG=\"%s\" surface=%dx%d scale=%dx\n", frmPath, pngPath, bgSurface->w, bgSurface->h, gHiResScale);
            }
        }
    }
#endif
    if (!usedHiResBackground && artPngLoadIndexed(backgroundFid, &bgPng, &bgW, &bgH)) {
        const char* frmPath = artBuildFilePath(backgroundFid);
        if (frmPath && *frmPath) {
            char pngPath[COMPAT_MAX_PATH];
            strncpy(pngPath, frmPath, sizeof(pngPath) - 1);
            pngPath[sizeof(pngPath) - 1] = '\0';
            char* dot = strrchr(pngPath, '.');
            if (dot) strcpy(dot, ".png"); else if (strlen(pngPath) + 4 < sizeof(pngPath)) strcat(pngPath, ".png");
            debugPrint("MM: background FRM=\"%s\" PNG=\"%s\" -> loaded %dx%d\n", frmPath, pngPath, bgW, bgH);
        }
        // Draw FRM background first to fully cover the window, then overlay the PNG centered.
        if (!usedHiResBackground) {
            {
                FrmImage base;
                if (base.lock(backgroundFid)) {
                    blitBufferToBuffer(base.getData(), 640, 480, 640, gMainMenuWindowBuffer, 640);
                    base.unlock();
                } else {
                    // As a fallback, clear to 0.
                    memset(gMainMenuWindowBuffer, 0, 640 * 480);
                }
            }

            // If PNG logical size doesn't match 640x480, center it on top of the base.
            int copyW = bgW;
            int copyH = bgH;
            if (copyW > 640) copyW = 640;
            if (copyH > 480) copyH = 480;
            int dstX = (640 - copyW) / 2;
            int dstY = (480 - copyH) / 2;
            blitBufferToBuffer(bgPng, copyW, copyH, bgW, gMainMenuWindowBuffer + dstY * 640 + dstX, 640);
            internal_free(bgPng);
        }
    } else {
        // If PNG exists but failed to load, hint about SDL2_image.
        if (!usedHiResBackground) {
            const ArtTextureMeta* meta = artTextureGetMeta(backgroundFid);
            if (meta && meta->exists) {
#ifdef HAVE_SDL2_IMAGE
                debugPrint("MM: PNG override present but failed to load via SDL2_image. Falling back to FRM.\n");
#else
                debugPrint("MM: PNG override present but SDL2_image support is not compiled. Falling back to FRM.\n");
#endif
            }
        }
        if (!usedHiResBackground && !_mainMenuBackgroundFrmImage.lock(backgroundFid)) {
            // NOTE: Uninline.
            return main_menu_fatal_error();
        }
        if (!usedHiResBackground) {
            // If hi-res mode is enabled but no PNG is available, build a truecolor background by
            // upscaling the FRM 2x (or HIRES_SCALE) using nearest-neighbor and set it.
            if (gHiResEnabled) {
                int scale = gHiResScale > 1 ? gHiResScale : 2;
                int srcW = 640, srcH = 480;
                const unsigned char* srcIdx = _mainMenuBackgroundFrmImage.getData();

                // Create destination surface (RGBA32) at scaled size
                SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, srcW * scale, srcH * scale, 32, SDL_PIXELFORMAT_RGBA32);
                if (dst) {
                    // Use current SDL palette for index->RGBA mapping
                    SDL_Palette* pal = (gSdlSurface && gSdlSurface->format) ? gSdlSurface->format->palette : nullptr;
                    const SDL_Color* colors = pal ? pal->colors : nullptr;
                    // Fallback: gray ramp if palette missing
                    SDL_Color fallback[256];
                    if (colors == nullptr) {
                        for (int i = 0; i < 256; ++i) { fallback[i].r = fallback[i].g = fallback[i].b = (Uint8)i; fallback[i].a = 255; }
                        colors = fallback;
                    }

                    Uint32* dp = (Uint32*)dst->pixels;
                    int dpitch = dst->pitch / 4;
                    for (int y = 0; y < srcH; ++y) {
                        const unsigned char* srow = srcIdx + y * srcW;
                        for (int x = 0; x < srcW; ++x) {
                            const SDL_Color c = colors[srow[x]];
                            Uint32 rgba = (Uint32)c.a << 24 | (Uint32)c.r << 16 | (Uint32)c.g << 8 | (Uint32)c.b;
                            // Replicate pixel into scale x scale block
                            int dx0 = x * scale;
                            int dy0 = y * scale;
                            for (int yy = 0; yy < scale; ++yy) {
                                Uint32* drow = dp + (dy0 + yy) * dpitch + dx0;
                                for (int xx = 0; xx < scale; ++xx) {
                                    drow[xx] = rgba;
                                }
                            }
                        }
                    }
                    hiResSetBackground(dst);
                    usedHiResBackground = true;
                    usedHiResBackgroundFromPng = false;
                    // Clear logical buffer so background shows through
                    memset(gMainMenuWindowBuffer, 0, 640 * 480);

                    const char* frmPath = artBuildFilePath(backgroundFid);
                    if (frmPath && *frmPath) {
                        char pngPath[COMPAT_MAX_PATH];
                        strncpy(pngPath, frmPath, sizeof(pngPath) - 1);
                        pngPath[sizeof(pngPath) - 1] = '\0';
                        char* dot = strrchr(pngPath, '.');
                        if (dot) strcpy(dot, ".png"); else if (strlen(pngPath) + 4 < sizeof(pngPath)) strcat(pngPath, ".png");
                        debugPrint("MM: no PNG, using upscaled FRM as hi-res background FRM=\"%s\" PNG=\"%s\" scale=%dx\n", frmPath, pngPath, scale);
                    }
                } else {
                    // Fallback to original FRM blit if surface creation failed
                    blitBufferToBuffer(_mainMenuBackgroundFrmImage.getData(), 640, 480, 640, gMainMenuWindowBuffer, 640);
                }
            } else {
                // Non-hires: draw FRM as usual
                blitBufferToBuffer(_mainMenuBackgroundFrmImage.getData(), 640, 480, 640, gMainMenuWindowBuffer, 640);
            }
            _mainMenuBackgroundFrmImage.unlock();
        } else {
            // Hi-res background in use; clear to 0 so it shows through.
            memset(gMainMenuWindowBuffer, 0, 640 * 480);
        }
        const char* frmPath = artBuildFilePath(backgroundFid);
        if (frmPath && *frmPath) {
            char pngPath[COMPAT_MAX_PATH];
            strncpy(pngPath, frmPath, sizeof(pngPath) - 1);
            pngPath[sizeof(pngPath) - 1] = '\0';
            char* dot = strrchr(pngPath, '.');
            if (dot) strcpy(dot, ".png"); else if (strlen(pngPath) + 4 < sizeof(pngPath)) strcat(pngPath, ".png");
            if (!usedHiResBackground) {
                debugPrint("MM: using FRM background (no PNG) FRM=\"%s\" PNG=\"%s\"\n", frmPath, pngPath);
            } else {
                debugPrint("MM: hi-res mode active, using truecolor background, skipping FRM draw FRM=\"%s\" PNG=\"%s\"\n", frmPath, pngPath);
            }
        }
    }

    int oldFont = fontGetCurrent();
    fontSetCurrent(100);

    // SFALL: Allow to change font color/flags of copyright/version text
    //        It's the last byte ('3C' by default) that picks the colour used. The first byte supplies additional flags for this option
    //        0x010000 - change the color for version string only
    //        0x020000 - underline text (only for the version string)
    //        0x040000 - monospace font (only for the version string)
    int fontSettings = _colorTable[21091], fontSettingsSFall = 0;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_MAIN_MENU_FONT_COLOR_KEY, &fontSettingsSFall);
    if (fontSettingsSFall && !(fontSettingsSFall & 0x010000))
        fontSettings = fontSettingsSFall & 0xFF;

    // SFALL: Allow to move copyright text
    int offsetX = 0, offsetY = 0;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_MAIN_MENU_CREDITS_OFFSET_X_KEY, &offsetX);
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_MAIN_MENU_CREDITS_OFFSET_Y_KEY, &offsetY);

    // Copyright.
    msg.num = 20;
    if (messageListGetItem(&gMiscMessageList, &msg)) {
        windowDrawText(gMainMenuWindow, msg.text, 0, offsetX + 15, offsetY + 460, fontSettings | 0x06000000);
    }

    // SFALL: Make sure font settings are applied when using 0x010000 flag
    if (fontSettingsSFall)
        fontSettings = fontSettingsSFall;

    // TODO: Allow to move version text
    // Version.
    char version[VERSION_MAX];
    versionGetVersion(version, sizeof(version));
    len = fontGetStringWidth(version);
    windowDrawText(gMainMenuWindow, version, 0, 615 - len, 460, fontSettings | 0x06000000);

    // menuup.frm
    fid = buildFid(OBJ_TYPE_INTERFACE, 299, 0, 0, 0);
    if (!_mainMenuButtonNormalFrmImage.lock(fid)) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }
    // Try PNG override for normal button icon (will be downscaled to logical size by loader if needed).
    if (_usePngButtons) {
        artPngLoadIndexed(fid, &_mainMenuButtonNormalPng, &_mainMenuButtonPngW, &_mainMenuButtonPngH);
    }

    // menudown.frm
    fid = buildFid(OBJ_TYPE_INTERFACE, 300, 0, 0, 0);
    if (!_mainMenuButtonPressedFrmImage.lock(fid)) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }
    // Try PNG override for pressed button icon (ignore its dimensions; must match logical FRM size)
    int tmpW = 0, tmpH = 0;
    if (_usePngButtons) {
        artPngLoadIndexed(fid, &_mainMenuButtonPressedPng, &tmpW, &tmpH);
    }

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        gMainMenuButtons[index] = -1;
    }

    // SFALL: Allow to move menu buttons
    offsetX = offsetY = 0;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_MAIN_MENU_OFFSET_X_KEY, &offsetX);
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_MAIN_MENU_OFFSET_Y_KEY, &offsetY);

    // Ensure overlay transparency is effective so circular icons composite cleanly over hi-res background.
    // (gHiResOverlayTransparent defaults to true in svga.)

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        // Choose images: prefer PNG override when available, else fallback to FRM images.
        unsigned char* upImg = ((_usePngButtons && _mainMenuButtonNormalPng) ? _mainMenuButtonNormalPng : _mainMenuButtonNormalFrmImage.getData());
        unsigned char* dnImg = ((_usePngButtons && _mainMenuButtonPressedPng) ? _mainMenuButtonPressedPng : _mainMenuButtonPressedFrmImage.getData());

        gMainMenuButtons[index] = buttonCreate(gMainMenuWindow,
            offsetX + 30,
            offsetY + 19 + index * 42 - index,
            26,
            26,
            -1,
            -1,
            1111,
            gMainMenuButtonKeyBindings[index],
            upImg,
            dnImg,
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (gMainMenuButtons[index] == -1) {
            // NOTE: Uninline.
            return main_menu_fatal_error();
        }

        buttonSetMask(gMainMenuButtons[index], _mainMenuButtonNormalFrmImage.getData());
    }

    // SFALL: Allow to change font color of buttons
    fontSettings = _colorTable[21091];
    fontSettingsSFall = 0;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_MAIN_MENU_BIG_FONT_COLOR_KEY, &fontSettingsSFall);
    if (fontSettingsSFall)
        fontSettings = fontSettingsSFall & 0xFF;

    {
        for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
            msg.num = 9 + index;
            if (messageListGetItem(&gMiscMessageList, &msg)) {
                len = fontGetStringWidth(msg.text);
                fontDrawText(gMainMenuWindowBuffer + offsetX + 640 * (offsetY + 42 * index - index + 20) + 126 - (len / 2), msg.text, 640 - (126 - (len / 2)) - 1, 640, fontSettings);
            }
        }
    }

    fontSetCurrent(oldFont);

    gMainMenuWindowInitialized = true;
    gMainMenuWindowHidden = true;

    return 0;
}

// 0x481968
void mainMenuWindowFree()
{
    if (!gMainMenuWindowInitialized) {
        return;
    }

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        // FIXME: Why it tries to free only invalid buttons?
        if (gMainMenuButtons[index] == -1) {
            buttonDestroy(gMainMenuButtons[index]);
        }
    }

    _mainMenuButtonPressedFrmImage.unlock();
    _mainMenuButtonNormalFrmImage.unlock();

    if (_mainMenuButtonNormalPng) { internal_free(_mainMenuButtonNormalPng); _mainMenuButtonNormalPng = nullptr; }
    if (_mainMenuButtonPressedPng) { internal_free(_mainMenuButtonPressedPng); _mainMenuButtonPressedPng = nullptr; }

    if (gMainMenuWindow != -1) {
        windowDestroy(gMainMenuWindow);
    }

    gMainMenuWindowInitialized = false;
}

// 0x481A00
void mainMenuWindowHide(bool animate)
{
    if (!gMainMenuWindowInitialized) {
        return;
    }

    if (gMainMenuWindowHidden) {
        return;
    }

    soundContinueAll();

    if (animate) {
        paletteFadeTo(gPaletteBlack);
        soundContinueAll();
    }

    windowHide(gMainMenuWindow);

    gMainMenuWindowHidden = true;
}

// 0x481A48
void mainMenuWindowUnhide(bool animate)
{
    if (!gMainMenuWindowInitialized) {
        return;
    }

    if (!gMainMenuWindowHidden) {
        return;
    }

    windowShow(gMainMenuWindow);

    if (animate) {
        colorPaletteLoad("color.pal");
        paletteFadeTo(_cmap);
    }

    gMainMenuWindowHidden = false;
}

// 0x481AA8
int _main_menu_is_enabled()
{
    return 1;
}

// 0x481AEC
int mainMenuWindowHandleEvents()
{
    _in_main_menu = true;

    bool oldCursorIsHidden = cursorIsHidden();
    if (oldCursorIsHidden) {
        mouseShowCursor();
    }

    unsigned int tick = getTicks();

    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();

        for (int buttonIndex = 0; buttonIndex < MAIN_MENU_BUTTON_COUNT; buttonIndex++) {
            if (keyCode == gMainMenuButtonKeyBindings[buttonIndex] || keyCode == toupper(gMainMenuButtonKeyBindings[buttonIndex])) {
                // NOTE: Uninline.
                main_menu_play_sound("nmselec1");

                rc = _return_values[buttonIndex];

                if (buttonIndex == MAIN_MENU_BUTTON_CREDITS && (gPressedPhysicalKeys[SDL_SCANCODE_RSHIFT] != KEY_STATE_UP || gPressedPhysicalKeys[SDL_SCANCODE_LSHIFT] != KEY_STATE_UP)) {
                    rc = MAIN_MENU_QUOTES;
                }

                break;
            }
        }

        if (rc == -1) {
            if (keyCode == KEY_CTRL_R) {
                rc = MAIN_MENU_SELFRUN;
                continue;
            } else if (keyCode == KEY_PLUS || keyCode == KEY_EQUAL) {
                brightnessIncrease();
            } else if (keyCode == KEY_MINUS || keyCode == KEY_UNDERSCORE) {
                brightnessDecrease();
            } else if (keyCode == KEY_UPPERCASE_D || keyCode == KEY_LOWERCASE_D) {
                rc = MAIN_MENU_SCREENSAVER;
                continue;
            } else if (keyCode == 1111) {
                if (!(mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT)) {
                    // NOTE: Uninline.
                    main_menu_play_sound("nmselec0");
                }
                continue;
            }
        }

        if (keyCode == KEY_ESCAPE || _game_user_wants_to_quit == 3) {
            rc = MAIN_MENU_EXIT;

            // NOTE: Uninline.
            main_menu_play_sound("nmselec1");
            break;
        } else if (_game_user_wants_to_quit == 2) {
            _game_user_wants_to_quit = 0;
        } else {
            if (getTicksSince(tick) >= gMainMenuScreensaverDelay) {
                rc = MAIN_MENU_TIMEOUT;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (oldCursorIsHidden) {
        mouseHideCursor();
    }

    _in_main_menu = false;

    return rc;
}

// NOTE: Inlined.
//
// 0x481C88
static int main_menu_fatal_error()
{
    mainMenuWindowFree();

    return -1;
}

// NOTE: Inlined.
//
// 0x481C94
static void main_menu_play_sound(const char* fileName)
{
    soundPlayFile(fileName);
}

} // namespace fallout
