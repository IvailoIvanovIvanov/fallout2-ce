#ifndef FPS_LIMITER_H
#define FPS_LIMITER_H

#include <cstdint>

namespace fallout {

// Phase 7: High-precision FPS limiter using performance counter
// When VSync is enabled, throttle() becomes a no-op since the GPU already limits us
class FpsLimiter {
public:
    FpsLimiter(unsigned int fps = 60);
    void mark();
    void throttle() const;
    void setFps(unsigned int fps);
    unsigned int getFps() const { return _fps; }
    
    // Phase 7c: VSync awareness - when VSync is on, throttle becomes a no-op
    void setVSyncEnabled(bool enabled) { _vsyncEnabled = enabled; }
    bool isVSyncEnabled() const { return _vsyncEnabled; }

private:
    unsigned int _fps;
    double _frameTimeUs;  // Frame time in microseconds
    mutable uint64_t _startCounter;
    uint64_t _counterFrequency;
    bool _vsyncEnabled;  // When true, throttle() is a no-op
};

} // namespace fallout

#endif /* FPS_LIMITER_H */
