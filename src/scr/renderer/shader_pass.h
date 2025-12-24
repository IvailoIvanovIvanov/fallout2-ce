#ifndef FALLOUT_RENDERER_SHADER_PASS_H
#define FALLOUT_RENDERER_SHADER_PASS_H

#include "gpu_context.h"
#include "render_types.h"

namespace fallout {
namespace renderer {

class ShaderPass {
public:
    virtual ~ShaderPass() = default;

    virtual bool Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) = 0;
    virtual void Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) = 0;
    virtual void Shutdown(GpuContext& context) = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SHADER_PASS_H
