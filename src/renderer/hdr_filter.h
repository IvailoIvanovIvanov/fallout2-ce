#ifndef FALLOUT_RENDERER_HDR_FILTER_H
#define FALLOUT_RENDERER_HDR_FILTER_H

#include "shader_pass.h"

namespace fallout {
namespace renderer {

class HDRFilter : public ShaderPass {
public:
    HDRFilter();
    ~HDRFilter() override;

    bool Init(GpuContext& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(GpuContext& context, void* input, void* output) override;
    void Shutdown(GpuContext& context) override;

    void SetParams(float saturation, float contrast, float blackCrushThreshold, float blackCrushStrength);

private:
    void* mShader = nullptr;
    int mInputWidth = 0;
    int mInputHeight = 0;

    float mSaturation = 1.0f;
    float mContrast = 1.0f;
    float mBlackCrushThreshold = 0.0f;
    float mBlackCrushStrength = 0.0f;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_HDR_FILTER_H
