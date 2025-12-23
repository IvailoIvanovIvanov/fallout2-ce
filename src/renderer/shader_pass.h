#ifndef FALLOUT_RENDERER_SHADER_PASS_H
#define FALLOUT_RENDERER_SHADER_PASS_H

#include "gpu_context.h"

namespace fallout {
namespace renderer {

class ShaderPass {
public:
    virtual ~ShaderPass() = default;

    virtual bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) = 0;
    virtual void Execute(GpuContext& context, void* input, void* output) = 0;
    virtual void Shutdown(GpuContext& context) = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SHADER_PASS_H
