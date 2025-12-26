#ifndef FALLOUT_RENDERER_SCREENSHOT_MANAGER_H
#define FALLOUT_RENDERER_SCREENSHOT_MANAGER_H

/**
 * @file screenshot_manager.h
 * @brief Pipeline debug screenshot capture system.
 *
 * ScreenshotManager provides functionality to capture intermediate
 * render pass outputs for debugging and quality analysis.
 * Screenshots are saved as PNG files with descriptive stage names.
 */

#include "gpu_context.h"
#include "render_types.h"
#include <string>

namespace fallout {
namespace renderer {

/**
 * @class ScreenshotManager
 * @brief Captures and saves GPU render surface contents.
 *
 * Used for debugging the rendering pipeline by capturing
 * the output of each pass. Screenshots are triggered by
 * the F8 key and saved to a timestamped folder.
 *
 * Usage:
 * @code
 *   manager.RequestCapture();
 *   // During frame:
 *   if (manager.IsCaptureRequested()) {
 *       manager.Capture(context, surface, "PassName");
 *   }
 *   manager.EndCapture();
 * @endcode
 */
class ScreenshotManager {
public:
    ScreenshotManager();

    //-------------------------------------------------------------------------
    // Capture Control
    //-------------------------------------------------------------------------

    /**
     * @brief Requests a screenshot capture on the next frame.
     */
    void RequestCapture() { mCaptureRequested = true; }

    /**
     * @brief Checks if a capture has been requested.
     */
    bool IsCaptureRequested() const { return mCaptureRequested; }

    /**
     * @brief Ends the current capture session.
     */
    void EndCapture() { mCaptureRequested = false; }

    //-------------------------------------------------------------------------
    // Capture Operations
    //-------------------------------------------------------------------------

    /**
     * @brief Captures a render surface to a PNG file.
     * @param context GPU context for readback operations.
     * @param surface The render surface to capture.
     * @param stageName Descriptive name for the file (e.g., "02_Anime4K_Upscale").
     */
    void Capture(GpuContext& context, const RenderSurface& surface, const std::string& stageName);

private:
    /**
     * @brief Saves pixel data to a PNG file.
     * @param data RGBA pixel data.
     * @param width Image width.
     * @param height Image height.
     * @param filename Output filename.
     */
    void SaveSurface(const void* data, int width, int height, const std::string& filename);

    bool mCaptureRequested = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SCREENSHOT_MANAGER_H
