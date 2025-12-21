#ifndef FALLOUT_RENDERER_ML_UPSCALE_PASS_H
#define FALLOUT_RENDERER_ML_UPSCALE_PASS_H

#include "ShaderPass.h"
#include "../upscaler_ml.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace fallout {
namespace renderer {

class MlUpscalePass : public ShaderPass {
public:
    MlUpscalePass();
    ~MlUpscalePass() override;

    bool Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) override;
    void Execute(D3D12Context& context, BufferManager& buffers) override;
    void Shutdown() override;

    void SetModelFile(const std::string& path) { mModelFile = path; }

private:
    UpscalerML mUpscaler;
    std::string mModelFile = "realesrgan-x4plus.onnx";
    
    // Intermediate resources for Tensor conversion
    Microsoft::WRL::ComPtr<ID3D12Resource> mInputTensor;
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTensor;
    
    // Compute Shaders for conversion
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPsoRgbaToPlanar;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPsoPlanarToRgba;
    
    // Descriptor Heap for conversion shaders
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
    
    int mWidth = 0;
    int mHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_ML_UPSCALE_PASS_H
