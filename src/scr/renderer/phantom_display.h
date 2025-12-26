#ifndef FALLOUT_RENDERER_PHANTOM_DISPLAY_H
#define FALLOUT_RENDERER_PHANTOM_DISPLAY_H

/**
 * @file phantom_display.h
 * @brief CPU-side frame buffer for GPU upload.
 *
 * PhantomDisplay acts as the staging area for game frame data
 * before it's uploaded to the GPU. It handles conversion from
 * indexed (palette-based) format to RGBA for GPU processing.
 */

#include <cstdint>
#include <vector>
#include <cstring>

namespace fallout {
namespace renderer {

/**
 * @enum PixelFormat
 * @brief Pixel format of the stored frame data.
 */
enum class PixelFormat {
    Indexed8,   ///< 8-bit indexed with palette (game native)
    RGBA8888    ///< 32-bit RGBA (GPU-ready)
};

/**
 * @class PhantomDisplay
 * @brief CPU frame buffer that stages data for GPU upload.
 *
 * The game renders to an indexed (8-bit palette) surface. This class
 * receives that data, converts it to RGBA using the current palette,
 * and stores it ready for GPU upload.
 *
 * The term "phantom" indicates this is a virtual display that the
 * game thinks it's rendering to, while the actual output goes through
 * the GPU rendering pipeline.
 */
class PhantomDisplay {
public:
    /**
     * @brief Creates a phantom display with the specified dimensions.
     * @param width Display width in pixels.
     * @param height Display height in pixels.
     */
    PhantomDisplay(int width, int height)
        : mWidth(width), mHeight(height) {
        mPixels.resize(width * height * 4);  // RGBA = 4 bytes per pixel
    }

    //-------------------------------------------------------------------------
    // Dimension Management
    //-------------------------------------------------------------------------

    /**
     * @brief Resizes the frame buffer.
     * @param width New width.
     * @param height New height.
     */
    void Resize(int width, int height) {
        if (mWidth == width && mHeight == height) return;
        mWidth = width;
        mHeight = height;
        mPixels.resize(width * height * 4);
    }

    //-------------------------------------------------------------------------
    // Data Input
    //-------------------------------------------------------------------------

    /**
     * @brief Sets frame data from indexed (8-bit) source with palette.
     *
     * Converts each indexed pixel to RGBA using the provided palette.
     * Alpha is forced to 255 for all pixels.
     *
     * @param indexedData 8-bit indexed pixel data.
     * @param palette 256-entry RGBA palette (0xAABBGGRR format).
     */
    void SetData(const uint8_t* indexedData, const uint32_t* palette) {
        mFormat = PixelFormat::RGBA8888;
        uint32_t* dest = reinterpret_cast<uint32_t*>(mPixels.data());
        for (int i = 0; i < mWidth * mHeight; ++i) {
            uint32_t color = palette[indexedData[i]];
            dest[i] = (color & 0x00FFFFFF) | 0xFF000000;  // Force alpha = 255
        }
    }

    /**
     * @brief Sets frame data from RGBA source.
     * @param rgbaData 32-bit RGBA pixel data.
     */
    void SetData(const uint32_t* rgbaData) {
        mFormat = PixelFormat::RGBA8888;
        std::memcpy(mPixels.data(), rgbaData, mWidth * mHeight * 4);
    }

    //-------------------------------------------------------------------------
    // Accessors
    //-------------------------------------------------------------------------

    /** @brief Returns pointer to the RGBA pixel data. */
    const void* GetPixels() const { return mPixels.data(); }

    /** @brief Returns the display width. */
    int GetWidth() const { return mWidth; }

    /** @brief Returns the display height. */
    int GetHeight() const { return mHeight; }

    /** @brief Returns the current pixel format. */
    PixelFormat GetFormat() const { return mFormat; }

private:
    int mWidth;
    int mHeight;
    std::vector<uint8_t> mPixels;
    PixelFormat mFormat = PixelFormat::RGBA8888;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_PHANTOM_DISPLAY_H
