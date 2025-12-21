#ifndef FALLOUT_RENDERER_BLUR_FILTER_H
#define FALLOUT_RENDERER_BLUR_FILTER_H

#include "shader_pass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace fallout {
namespace renderer {

class BlurFilter : public ShaderPass {
public:
    BlurFilter();
    ~BlurFilter() override;

    bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(D3D12Context& context, BufferManager& buffers) override;
    void Shutdown() override;

    void SetStrength(float strength);

private:
    struct BlurParams {
        uint32_t resolution[2];
        float strength;
        float padding;
    };

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
    
    BlurParams mParams = {};
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_BLUR_FILTER_H
