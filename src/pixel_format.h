#ifndef PIXEL_FORMAT_H
#define PIXEL_FORMAT_H

namespace fallout {

enum class PixelFormat {
    Indexed8,
    Argb8888,
};

inline int pixelFormatBytesPerPixel(PixelFormat format)
{
    switch (format) {
    case PixelFormat::Argb8888:
        return 4;
    case PixelFormat::Indexed8:
    default:
        return 1;
    }
}

inline bool pixelFormatHasAlpha(PixelFormat format)
{
    return format == PixelFormat::Argb8888;
}

} // namespace fallout

#endif /* PIXEL_FORMAT_H */
