#ifndef FALLOUT_RENDERER_PREPROCESSING_PASS_H
#define FALLOUT_RENDERER_PREPROCESSING_PASS_H

#include "ShaderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace fallout {
namespace renderer {

class PreprocessingPass : public ShaderPass {
public:
    PreprocessingPass();
    ~PreprocessingPass() override;

    bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(D3D12Context& context, BufferManager& buffers) override;
    void Shutdown() override;

    void SetParams(float blur, float saturation, float contrast);

private:
    struct PreprocessParams {
        uint32_t resolution[2];
        float blurStrength;
        float hdrSaturation;
        float hdrContrast;
        float padding[2];
    };

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
    
    PreprocessParams mParams = {};
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_PREPROCESSING_PASS_H
