#ifndef FALLOUT_RENDERER_RENDER_TYPES_H
#define FALLOUT_RENDERER_RENDER_TYPES_H

namespace fallout {
namespace renderer {

enum class TextureFormat {
    RGBA8,
    RGBA16F,
    RGBA32F
};

struct TextureDesc {
    int width;
    int height;
    TextureFormat format;
};

struct RenderSurface {
    void* handle = nullptr;
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_TYPES_H
