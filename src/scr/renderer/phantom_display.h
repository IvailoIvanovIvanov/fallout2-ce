#ifndef FALLOUT_RENDERER_PHANTOM_DISPLAY_H
#define FALLOUT_RENDERER_PHANTOM_DISPLAY_H

#include <cstdint>
#include <vector>
#include <cstring>

namespace fallout {
namespace renderer {

enum class PixelFormat {
    Indexed8,
    RGBA8888
};

class PhantomDisplay {
public:
    PhantomDisplay(int width, int height) 
        : mWidth(width), mHeight(height) {
        // Allocate for RGBA (4 bytes per pixel)
        mPixels.resize(width * height * 4); 
    }

    void Resize(int width, int height) {
        if (mWidth == width && mHeight == height) return;
        mWidth = width;
        mHeight = height;
        mPixels.resize(width * height * 4);
    }

    void SetData(const uint8_t* indexedData, const uint32_t* palette) {
        mFormat = PixelFormat::RGBA8888; // We convert to RGBA internally
        uint32_t* dest = reinterpret_cast<uint32_t*>(mPixels.data());
        for (int i = 0; i < mWidth * mHeight; ++i) {
            uint32_t color = palette[indexedData[i]];
            // Ensure alpha is 255 (0xFF)
            dest[i] = (color & 0x00FFFFFF) | 0xFF000000;
        }
    }

    void SetData(const uint32_t* rgbaData) {
        mFormat = PixelFormat::RGBA8888;
        std::memcpy(mPixels.data(), rgbaData, mWidth * mHeight * 4);
    }

    const void* GetPixels() const { return mPixels.data(); }
    int GetWidth() const { return mWidth; }
    int GetHeight() const { return mHeight; }
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
