#ifndef FALLOUT_RENDERER_RENDER_TYPES_H
#define FALLOUT_RENDERER_RENDER_TYPES_H

namespace fallout {
namespace renderer {

struct RenderSurface {
    void* handle = nullptr;
    int width = 0;
    int height = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_TYPES_H
