#ifndef FALLOUT_RENDERER_BLUR_FILTER_H
#define FALLOUT_RENDERER_BLUR_FILTER_H

#include "shader_pass.h"

namespace fallout {
namespace renderer {

class BlurFilter : public ShaderPass {
public:
    BlurFilter();
    ~BlurFilter() override;

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

    void SetStrength(float strength);

private:
    struct BlurParams {
        uint32_t resolution[2];
        float strength;
        float padding;
    };

    void* mShader = nullptr;
    
    BlurParams mParams = {};
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_BLUR_FILTER_H
