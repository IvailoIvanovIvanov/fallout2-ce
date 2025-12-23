#ifndef FALLOUT_RENDERER_REAL_DISPLAY_H
#define FALLOUT_RENDERER_REAL_DISPLAY_H

namespace fallout {
namespace renderer {

class RealDisplay {
public:
    RealDisplay(int width, int height) 
        : mWidth(width), mHeight(height) {}

    void Resize(int width, int height) {
        mWidth = width;
        mHeight = height;
    }

    int GetWidth() const { return mWidth; }
    int GetHeight() const { return mHeight; }

private:
    int mWidth;
    int mHeight;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_REAL_DISPLAY_H
