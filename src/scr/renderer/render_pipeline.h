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
#include <string>

struct SDL_Window;
struct SDL_Surface;

namespace fallout {
namespace renderer {

enum class RenderMode {
    SIMPLE = 0,
    ANIME4K = 1
};

class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    // Initialize the full pipeline, including displays and passes
    bool Init(int inputWidth, int inputHeight, int outputWidth, int outputHeight, SDL_Window* window);
    void Shutdown();

    // Reconfigure output resolution
    bool Reconfigure(int outputWidth, int outputHeight);

    // Set input data
    bool SetIndexedInput(SDL_Surface* surface);
    bool SetRgbaInput(const uint32_t* rgbaBuffer);

    // Execute the pipeline
    void Dispatch();

    // Returns the pointer to the readback buffer (Frame N-1)
    const void* GetOutput();

    // Configuration
    void LoadConfiguration();
    void SetMode(RenderMode mode) { mConfiguredMode = mode; }

private:
    bool CreateIntermediateBuffers();
    void SetupPasses();
    void LogDiagnostic(const char* format, ...);

    std::unique_ptr<GpuContext> mContext;
    BufferManager mBuffers;
    
    // Displays
    std::unique_ptr<PhantomDisplay> mPhantomDisplay;
    std::unique_ptr<RealDisplay> mRealDisplay;
    SDL_Window* mWindow = nullptr;

    // Passes
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

    // Configuration
    RenderMode mMode = RenderMode::SIMPLE;
    RenderMode mConfiguredMode = RenderMode::ANIME4K;
    
    // Filter Settings
    float mSharpness = 0.5f;
    bool mEnableDebanding = true;
    float mDebandingStrength = 0.5f;
    bool mEnableEdgeSmoothing = true;
    float mSmoothingStrength = 0.6f;
    bool mEnableKuwahara = false;
    int mKuwaharaRadius = 2;
    bool mEnableSoftHDR = true;
    float mHdrStrength = 0.5f;
    float mHdrSaturation = 1.5f;
    float mHdrContrast = 1.3f;
    float mBlackCrushThreshold = 0.05f;
    float mBlackCrushStrength = 1.5f;
    
    // Post-Processing Config
    bool mEnablePostSharpen = false;
    bool mEnablePostDenoise = false;
    
    bool mVerboseLogging = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
