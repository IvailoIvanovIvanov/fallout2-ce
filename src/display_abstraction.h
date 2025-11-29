#ifndef FALLOUT_DISPLAY_ABSTRACTION_H_
#define FALLOUT_DISPLAY_ABSTRACTION_H_

#include "geometry.h"

namespace fallout {

// Logical coordinates are always expressed in the phantom display space
// (typically 640x480). Physical coordinates are window pixels.
struct LogicalCoordinate {
    int x;
    int y;
};

struct PhysicalCoordinate {
    int x;
    int y;
};

// Forward declarations of lightweight views used by wrappers.
struct RenderCommandBufferView;

// Minimal abstraction interfaces. Implementations are thin wrappers over
// existing subsystems and do not change behavior.
class PhantomDisplay {
public:
    // Returns pointer to the phantom (virtual) 8-bit screen buffer.
    unsigned char* getScreenBuffer() const;
    // Returns the pitch (stride) of the phantom screen buffer.
    int getScreenPitch() const;
    // Invalidates a logical rectangle in the phantom display.
    void invalidateRect(const Rect& rect) const;
    // Invalidates the entire logical screen.
    void invalidateAll() const;
};

class RealDisplay {
public:
    // Processes render commands and updates physical true-color overlays.
    void processCommands(const RenderCommandBufferView& commands) const;
    // Presents the current frame via the SDL presenter.
    void present() const;
    // Query whether the orchestrator is enabled/owns the presenter.
    bool isOrchestratorEnabled() const;
    bool orchestratorOwnsPresenter() const;
};

} // namespace fallout

#endif /* FALLOUT_DISPLAY_ABSTRACTION_H_ */
