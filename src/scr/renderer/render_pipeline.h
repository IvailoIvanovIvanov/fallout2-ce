#ifndef FALLOUT_RENDERER_RENDER_PIPELINE_H
#define FALLOUT_RENDERER_RENDER_PIPELINE_H

#include "gpu_context.h"
#include "buffer_manager.h"
#include "shader_pass.h"
#include "scaler_pass.h"
#include "phantom_display.h"
#include "real_display.h"
#include "render_types.h"
#include "screenshot_manager.h"

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
    ScreenshotManager mScreenshotManager;
    bool mF8Pressed = false;
    
    // Displays
    std::unique_ptr<PhantomDisplay> mPhantomDisplay;
    std::unique_ptr<RealDisplay> mRealDisplay;
    SDL_Window* mWindow = nullptr;

    // Passes
    std::unique_ptr<ShaderPass> mScalerPass;

    int mInputWidth = 0;
    int mInputHeight = 0;
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    
    // Window dimensions (Physical Screen)
    int mWindowWidth = 0;
    int mWindowHeight = 0;
    
    // Render dimensions (Logical Output, Aspect Correct)
    int mRenderWidth = 0;
    int mRenderHeight = 0;

    bool mInitialized = false;

    // Configuration
    RenderMode mMode = RenderMode::SIMPLE;
    RenderMode mConfiguredMode = RenderMode::ANIME4K;
    
    // Filter Settings
    float mSharpness = 0.5f;
    bool mEnableDebanding = true;

    // Pipeline Configuration
    bool mEnablePrePass = false;
    std::string mPrePassShader;

    bool mEnableClean1 = true;
    std::string mClean1Shader;

    bool mEnableClean2 = true;
    std::string mClean2Shader;

    bool mEnableScale1 = true;
    std::string mScale1Shader;

    bool mEnableOptimize = true;
    std::string mOptimizeShader;

    bool mEnableScale2 = true;
    std::string mScale2Shader;

    bool mEnablePolish = true;
    std::string mPolishShader;

    bool mEnablePostPass = false;
    std::string mPostPassShader;

    // Scaling Buffers
    std::vector<RenderSurface> mScalingBuffers;
    std::vector<std::unique_ptr<ShaderPass>> mAnime4KPasses;
    std::unique_ptr<ShaderPass> mPrePass;
    std::unique_ptr<ShaderPass> mPostPass;
    float mDebandingStrength = 0.5f;
    
    bool mVerboseLogging = false;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_PIPELINE_H
