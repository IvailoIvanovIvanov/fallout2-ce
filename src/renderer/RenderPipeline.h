#ifndef FALLOUT_RENDERER_RENDER_PIPELINE_H
#define FALLOUT_RENDERER_RENDER_PIPELINE_H

#include "D3D12Context.h"
#include "BufferManager.h"
#include "ShaderPass.h"

#include <vector>
#include <memory>

namespace fallout {
namespace renderer {

class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    bool Init(int inputWidth, int inputHeight, int outputWidth, int outputHeight);
    void Shutdown();

    // Main entry point for the frame
    void Dispatch(const void* inputPixels);

    // Returns the pointer to the readback buffer (Frame N-1)
    const void* GetOutput();

    void AddPass(std::unique_ptr<ShaderPass> pass);

private:
    D3D12Context mContext;
    BufferManager mBuffers;
    std::vector<std::unique_ptr<ShaderPass>> mPasses;

    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    bool mInitialized = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
