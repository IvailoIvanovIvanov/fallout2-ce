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
#include "logger.h"

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
     * @param pitch Source row pitch in bytes (may be larger than width for alignment).
     */
    void SetData(const uint8_t* indexedData, const uint32_t* palette, int pitch) {
        mFormat = PixelFormat::RGBA8888;
        uint32_t* dest = reinterpret_cast<uint32_t*>(mPixels.data());
        
        // Log sample of input indexed data and palette lookup
        static int logCounter = 0;
        bool shouldLog = (logCounter++ % 60 == 0);  // Log every 60 frames
        
        if (shouldLog) {
            // Sample first 4 indexed pixels
            uint8_t idx0 = indexedData[0];
            uint8_t idx1 = indexedData[1];
            uint8_t idx2 = indexedData[2];
            uint8_t idx3 = indexedData[3];
            
            // Get corresponding palette colors
            uint32_t pal0 = palette[idx0];
            uint32_t pal1 = palette[idx1];
            uint32_t pal2 = palette[idx2];
            uint32_t pal3 = palette[idx3];
            
            Logger::Log(LogLevel::Info, "[PHANTOM] SetData: dims=%dx%d, pitch=%d", mWidth, mHeight, pitch);
            Logger::Log(LogLevel::Info, "[PHANTOM] Input indices[0-3]: %d, %d, %d, %d", idx0, idx1, idx2, idx3);
            Logger::Log(LogLevel::Info, "[PHANTOM] Palette[idx0=%d]=0x%08X, Palette[idx1=%d]=0x%08X", idx0, pal0, idx1, pal1);
            Logger::Log(LogLevel::Info, "[PHANTOM] Palette[idx2=%d]=0x%08X, Palette[idx3=%d]=0x%08X", idx2, pal2, idx3, pal3);
            
            // Also sample from middle of image to get more representative pixels
            int midY = mHeight / 2;
            int midX = mWidth / 2;
            int midIdx = midY * pitch + midX;
            if (midIdx < pitch * mHeight) {
                uint8_t midPalIdx = indexedData[midIdx];
                uint32_t midPalColor = palette[midPalIdx];
                Logger::Log(LogLevel::Info, "[PHANTOM] Mid-screen pixel[%d,%d]: index=%d, palette=0x%08X", midX, midY, midPalIdx, midPalColor);
            }
        }
        
        for (int y = 0; y < mHeight; ++y) {
            for (int x = 0; x < mWidth; ++x) {
                int srcIdx = y * pitch + x;
                int dstIdx = y * mWidth + x;
                uint32_t color = palette[indexedData[srcIdx]];
                dest[dstIdx] = (color & 0x00FFFFFF) | 0xFF000000;  // Force alpha = 255
            }
        }
        
        if (shouldLog) {
            // Log output after conversion
            Logger::Log(LogLevel::Info, "[PHANTOM] Output RGBA[0-3]: 0x%08X, 0x%08X, 0x%08X, 0x%08X", 
                        dest[0], dest[1], dest[2], dest[3]);
            
            // Sample output from middle
            int midDstIdx = (mHeight / 2) * mWidth + (mWidth / 2);
            Logger::Log(LogLevel::Info, "[PHANTOM] Output mid-screen RGBA: 0x%08X", dest[midDstIdx]);
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
