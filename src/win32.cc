#include "win32.h"

#include <stdlib.h>

#include <SDL.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "main.h"
#include "svga.h"
#include "window_manager.h"

#if __APPLE__ && TARGET_OS_IOS
#include "platform/ios/paths.h"
#endif

namespace fallout {

// 0x51E444
bool gProgramIsActive = false;

#ifdef _WIN32
HANDLE GNW95_mutex = nullptr;
#endif

int main(int argc, char* argv[])
{
    int rc;

#if _WIN32
    // Enable Per-Monitor DPI awareness so SDL gets the true native resolution
    // This must be done before any window creation (including SDL_Init)
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_AWARENESS_CONTEXT)-4)
    // We use SetProcessDpiAwarenessContext if available (Windows 10 1703+),
    // otherwise fall back to SetProcessDPIAware (Vista+)
    {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(void*);
        typedef BOOL (WINAPI *SetProcessDPIAwareProc)(void);
        
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32 != nullptr) {
            SetProcessDpiAwarenessContextProc setDpiContext = 
                (SetProcessDpiAwarenessContextProc)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setDpiContext != nullptr) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
                setDpiContext((void*)-4);
            } else {
                // Fallback for older Windows versions
                SetProcessDPIAwareProc setDpiAware = 
                    (SetProcessDPIAwareProc)GetProcAddress(user32, "SetProcessDPIAware");
                if (setDpiAware != nullptr) {
                    setDpiAware();
                }
            }
        }
    }

    GNW95_mutex = CreateMutexA(0, TRUE, "GNW95MUTEX");
    if (GetLastError() != ERROR_SUCCESS) {
        return 0;
    }
#endif

#if __APPLE__ && TARGET_OS_IOS
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    chdir(iOSGetDocumentsPath());
#endif

#if __APPLE__ && TARGET_OS_OSX
    char* basePath = SDL_GetBasePath();
    chdir(basePath);
    SDL_free(basePath);
#endif

#if __ANDROID__
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    chdir(SDL_AndroidGetExternalStoragePath());
#endif

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        return EXIT_FAILURE;
    }

    atexit(SDL_Quit);

    SDL_ShowCursor(SDL_DISABLE);

    gProgramIsActive = true;
    rc = falloutMain(argc, argv);

#if _WIN32
    CloseHandle(GNW95_mutex);
#endif

    return rc;
}

} // namespace fallout

int main(int argc, char* argv[])
{
    return fallout::main(argc, argv);
}
