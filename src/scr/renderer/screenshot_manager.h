#ifndef FALLOUT_RENDERER_SCREENSHOT_MANAGER_H
#define FALLOUT_RENDERER_SCREENSHOT_MANAGER_H

#include "gpu_context.h"
#include "render_types.h"
#include <string>

namespace fallout {
namespace renderer {

class ScreenshotManager {
public:
    ScreenshotManager();
    
    void RequestCapture() { mCaptureRequested = true; }
    bool IsCaptureRequested() const { return mCaptureRequested; }
    void EndCapture() { mCaptureRequested = false; }

    void Capture(GpuContext& context, const RenderSurface& surface, const std::string& stageName);

private:
    bool mCaptureRequested = false;
    void SaveSurface(const void* data, int width, int height, const std::string& filename);
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SCREENSHOT_MANAGER_H
