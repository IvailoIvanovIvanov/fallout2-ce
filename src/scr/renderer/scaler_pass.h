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

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

private:
    std::unique_ptr<Shader> mShader;

    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_SCALER_PASS_H
