#ifndef FALLOUT_RENDERER_SCALER_PASS_H
#define FALLOUT_RENDERER_SCALER_PASS_H

#include "shader_pass.h"
#include "shader.h"
#include <memory>

namespace fallout {
namespace renderer {

class ScalerPass : public ShaderPass {
public:
    ScalerPass();
    ~ScalerPass() override;

    bool Init(GpuContext& context, const RenderSurface& input, const RenderSurface& output) override;
    void Execute(GpuContext& context, const RenderSurface& input, const RenderSurface& output) override;
    void Shutdown(GpuContext& context) override;

private:
    std::unique_ptr<Shader> mShader;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SCALER_PASS_H
