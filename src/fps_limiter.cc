#include "fps_limiter.h"

#include <SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace fallout {

// Phase 7: High-precision FPS limiter
// Uses QueryPerformanceCounter on Windows, SDL_GetPerformanceCounter elsewhere
// Implements a spin-wait for final sub-millisecond precision

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps)
    , _frameTimeUs(fps > 0 ? 1000000.0 / fps : 0)
    , _startCounter(0)
    , _vsyncEnabled(false)
{
#ifdef _WIN32
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    _counterFrequency = freq.QuadPart;
#else
    _counterFrequency = SDL_GetPerformanceFrequency();
#endif
}

void FpsLimiter::setFps(unsigned int fps)
{
    _fps = fps;
    _frameTimeUs = fps > 0 ? 1000000.0 / fps : 0;
}

void FpsLimiter::mark()
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    _startCounter = counter.QuadPart;
#else
    _startCounter = SDL_GetPerformanceCounter();
#endif
}

void FpsLimiter::throttle() const
{
    // Phase 7c: When VSync is enabled, the GPU already limits frame rate
    // so we don't need to do any additional throttling
    if (_vsyncEnabled) {
        return;
    }
    
    if (_fps == 0 || _frameTimeUs <= 0) {
        return;
    }

#ifdef _WIN32
    LARGE_INTEGER currentCounter;
    QueryPerformanceCounter(&currentCounter);
    uint64_t elapsed = currentCounter.QuadPart - _startCounter;
#else
    uint64_t currentCounter = SDL_GetPerformanceCounter();
    uint64_t elapsed = currentCounter - _startCounter;
#endif

    // Convert elapsed to microseconds
    double elapsedUs = (elapsed * 1000000.0) / _counterFrequency;
    double remainingUs = _frameTimeUs - elapsedUs;

    if (remainingUs <= 0) {
        return;
    }

    // Sleep for most of the remaining time (leave 1.5ms for spin-wait)
    // SDL_Delay has ~15ms granularity on Windows, so we only use it for larger waits
    if (remainingUs > 2000) {
        unsigned int sleepMs = static_cast<unsigned int>((remainingUs - 1500) / 1000);
        if (sleepMs > 0) {
            SDL_Delay(sleepMs);
        }
    }

    // Spin-wait for precise timing (burns CPU but ensures accuracy)
    while (true) {
#ifdef _WIN32
        QueryPerformanceCounter(&currentCounter);
        elapsed = currentCounter.QuadPart - _startCounter;
#else
        currentCounter = SDL_GetPerformanceCounter();
        elapsed = currentCounter - _startCounter;
#endif
        elapsedUs = (elapsed * 1000000.0) / _counterFrequency;
        if (elapsedUs >= _frameTimeUs) {
            break;
        }
    }
}

} // namespace fallout
