#ifndef FPS_LIMITER_H
#define FPS_LIMITER_H

#include <cstdint>

namespace fallout {

// Phase 7: High-precision FPS limiter using performance counter
class FpsLimiter {
public:
    FpsLimiter(unsigned int fps = 60);
    void mark();
    void throttle() const;
    void setFps(unsigned int fps);
    unsigned int getFps() const { return _fps; }

private:
    unsigned int _fps;
    double _frameTimeUs;  // Frame time in microseconds
    mutable uint64_t _startCounter;
    uint64_t _counterFrequency;
};

} // namespace fallout

#endif /* FPS_LIMITER_H */
