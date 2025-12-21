#ifndef FALLOUT_RENDERER_SHADER_PASS_H
#define FALLOUT_RENDERER_SHADER_PASS_H

#include "d3d12_context.h"
#include "buffer_manager.h"

namespace fallout {
namespace renderer {

class ShaderPass {
public:
    virtual ~ShaderPass() = default;

    virtual bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) = 0;
    virtual void Execute(D3D12Context& context, BufferManager& buffers) = 0;
    virtual void Shutdown() = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SHADER_PASS_H
