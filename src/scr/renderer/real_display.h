#ifndef FALLOUT_RENDERER_REAL_DISPLAY_H
#define FALLOUT_RENDERER_REAL_DISPLAY_H

/**
 * @file real_display.h
 * @brief Represents the actual output display/window dimensions.
 *
 * RealDisplay tracks the physical window size that the rendered
 * output will be presented to. It's used for scaling calculations
 * and viewport setup during presentation.
 */

namespace fallout {
namespace renderer {

/**
 * @class RealDisplay
 * @brief Simple container for physical display dimensions.
 *
 * Unlike PhantomDisplay (which is the game's logical resolution),
 * RealDisplay represents the actual window or screen size that
 * the upscaled output will be presented to.
 */
class RealDisplay {
public:
    /**
     * @brief Creates a real display with specified dimensions.
     * @param width Window/screen width in pixels.
     * @param height Window/screen height in pixels.
     */
    RealDisplay(int width, int height)
        : mWidth(width), mHeight(height) {}

    //-------------------------------------------------------------------------
    // Dimension Management
    //-------------------------------------------------------------------------

    /**
     * @brief Updates the display dimensions (e.g., on window resize).
     * @param width New width.
     * @param height New height.
     */
    void Resize(int width, int height) {
        mWidth = width;
        mHeight = height;
    }

    //-------------------------------------------------------------------------
    // Accessors
    //-------------------------------------------------------------------------

    /** @brief Returns the display width. */
    int GetWidth() const { return mWidth; }

    /** @brief Returns the display height. */
    int GetHeight() const { return mHeight; }

private:
    int mWidth;
    int mHeight;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_REAL_DISPLAY_H
