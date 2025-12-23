#ifndef FALLOUT_RENDERER_ML_UPSCALE_PASS_H
#define FALLOUT_RENDERER_ML_UPSCALE_PASS_H

#include "shader_pass.h"

namespace fallout {
namespace renderer {

class MLUpscalePass : public ShaderPass {
public:
    MLUpscalePass();
    ~MLUpscalePass() override;

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

    void SetModelFile(const std::string& modelFile);

private:
    void* mShader = nullptr;
    int mInputWidth = 0;
    int mInputHeight = 0;
    std::string mModelFile;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_ML_UPSCALE_PASS_H
