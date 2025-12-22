#ifndef FALLOUT_RENDERER_ANIME4K_PASS_H
#define FALLOUT_RENDERER_ANIME4K_PASS_H

#include "shader_pass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace fallout {
namespace renderer {

class Anime4kPass : public ShaderPass {
public:
    Anime4kPass();
    ~Anime4kPass() override;

    bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(D3D12Context& context, ID3D12Resource* input, ID3D12Resource* output) override;
    void Shutdown() override;

    void SetStrength(float strength) { mStrength = strength; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;

    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    float mStrength = 0.5f;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_ANIME4K_PASS_H
