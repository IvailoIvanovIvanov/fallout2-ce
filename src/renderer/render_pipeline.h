#ifndef FALLOUT_RENDERER_RENDER_PIPELINE_H
#define FALLOUT_RENDERER_RENDER_PIPELINE_H

#include "gpu_context.h"
#include "buffer_manager.h"
#include "shader_pass.h"
#include "scaler_pass.h"
#include "phantom_display.h"
#include "real_display.h"

#include <vector>
#include <memory>

struct SDL_Window;

namespace fallout {
namespace renderer {

class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    bool Init(int inputWidth, int inputHeight, const RealDisplay& outputDisplay, SDL_Window* window);
    void Shutdown();

    // Main entry point for the frame
    void Dispatch(const PhantomDisplay& display);

    // Returns the pointer to the readback buffer (Frame N-1)
    const void* GetOutput();

    void AddPass(std::unique_ptr<ShaderPass> pass);
    void AddPostPass(std::unique_ptr<ShaderPass> pass);
    void SetScalerPass(std::unique_ptr<ShaderPass> pass);

private:
    bool CreateIntermediateBuffers();

    std::unique_ptr<GpuContext> mContext;
    BufferManager mBuffers;
    std::vector<std::unique_ptr<ShaderPass>> mPasses;
    std::vector<std::unique_ptr<ShaderPass>> mPostPasses;
    std::unique_ptr<ShaderPass> mScalerPass;

    // Phantom buffers for 640x480 processing
    void* mIntermediateBuffers[2] = { nullptr, nullptr };
    // Output buffers for WindowSize processing
    void* mPostIntermediateBuffers[2] = { nullptr, nullptr };

    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    bool mInitialized = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
