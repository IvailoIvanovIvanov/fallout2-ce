#ifndef FALLOUT_RENDERER_GENERIC_SHADER_PASS_H
#define FALLOUT_RENDERER_GENERIC_SHADER_PASS_H

#include "shader_pass.h"
#include <string>

namespace fallout {
namespace renderer {

class GenericShaderPass : public ShaderPass {
public:
    GenericShaderPass(const std::string& shaderPath);
    ~GenericShaderPass() override = default;

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

private:
    std::string mShaderPath;
    void* mComputeShader = nullptr;
    int mWidth = 0;
    int mHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_GENERIC_SHADER_PASS_H
