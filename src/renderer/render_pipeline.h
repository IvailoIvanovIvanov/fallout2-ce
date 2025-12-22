#ifndef FALLOUT_RENDERER_RENDER_PIPELINE_H
#define FALLOUT_RENDERER_RENDER_PIPELINE_H

#include "d3d12_context.h"
#include "buffer_manager.h"
#include "shader_pass.h"
#include "scaler_pass.h"
#include "phantom_display.h"
#include "real_display.h"

#include <vector>
#include <memory>

namespace fallout {
namespace renderer {

class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    bool Init(int inputWidth, int inputHeight, const RealDisplay& outputDisplay);
    void Shutdown();

    // Main entry point for the frame
    void Dispatch(const PhantomDisplay& display);

    // Returns the pointer to the readback buffer (Frame N-1)
    const void* GetOutput();

    void AddPass(std::unique_ptr<ShaderPass> pass);

private:
    bool CreateIntermediateBuffers();

    D3D12Context mContext;
    BufferManager mBuffers;
    std::vector<std::unique_ptr<ShaderPass>> mPasses;
    std::unique_ptr<ScalerPass> mScalerPass;

    // Phantom buffers for 640x480 processing
    Microsoft::WRL::ComPtr<ID3D12Resource> mIntermediateBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mIntermediateHeap; // For clearing/copying if needed

    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    bool mInitialized = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
