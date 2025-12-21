#ifndef FALLOUT_RENDERER_HDR_FILTER_H
#define FALLOUT_RENDERER_HDR_FILTER_H

#include "shader_pass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace fallout {
namespace renderer {

class HdrFilter : public ShaderPass {
public:
    HdrFilter();
    ~HdrFilter() override;

    bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(D3D12Context& context, BufferManager& buffers) override;
    void Shutdown() override;

    void SetParams(float saturation, float contrast);

private:
    struct HdrParams {
        uint32_t resolution[2];
        float saturation;
        float contrast;
        float padding[2];
    };

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
    
    HdrParams mParams = {};
    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_HDR_FILTER_H
